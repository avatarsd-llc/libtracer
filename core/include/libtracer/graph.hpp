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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <deque>
#include <expected>
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "libtracer/error.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path.hpp"
#include "libtracer/sink_slot.hpp"
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
 * (a `tr::net` concern — @ref graph_t::configure_remote_delivery_sink) interprets these:
 * it maps @ref link to a transport child and emits a full-route `FWD{WRITE}` or,
 * when @ref delivery_compact, an auto-promoted label `COMPACT` (RFC-0004 §D/§E.1).
 * @ref link is borrowed for the sink call only; @ref return_route is a refcount
 * clone of the stored route segment (ADR-0041 §2) — the sink may rope it into an
 * egress frame, and it stays alive across a concurrent unsubscribe.
 */
struct remote_delivery_t {
    std::string_view link; /**< @brief This node's NAME for the consumer link. */
    view_t return_route;   /**< @brief Consumer return route (PATH TLV view, refcount clone). */
    /** @brief Completed reverse bound route (`PATH_REF` view, refcount clone; empty ⇒
     *         canonical-only). Element 0 is this node's own reference, consumed locally by
     *         the sink per delivery — RFC-0024 §7.1 amendment 1. */
    view_t reverse_route;
    /** @brief The edge's stored ACL fan-in context (#81) — the subject the sink's local
     *         element-0 consumption re-checks §6.2 under. */
    std::string_view caller;
    bool delivery_compact = false; /**< @brief Opt-in to label-compacted delivery. */
};

/**
 * @brief The remote-delivery sink itself — what @ref graph_t::configure_remote_delivery_sink
 *        installs and the producer fan-out calls per remote subscription edge.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049) — see
 *       `subject_resolver_fn_t` for why. This is the one of the three on the WRITE hot
 *       path, so it is also the one where the `std::function` indirect call and its
 *       destroy-on-assign were most expensive; @p ctx is caller-owned (the
 *       `tr::net::fwd_router_t`) and must outlive every dispatch the graph can still make,
 *       exactly as `receiver_slot_t`'s context must.
 */
using remote_delivery_fn_t = void (*)(void* ctx, const remote_delivery_t& sub, const rope_t& value);

/**
 * @brief An opaque handle to ONE in-process subscription — the token @ref graph_t::unsubscribe
 *        removes it by (ADR-0049 host-SDK sugar for the wire `:subscribers[N]` clear).
 *
 * Returned by the callback-form @ref graph_t::subscribe overloads. It names a producer vertex and
 * one of that vertex's `:subscribers[]` slots; the vertex is pinned for the graph's lifetime
 * (ADR-0057 — vertices are never freed), so the handle stays valid until it is unsubscribed.
 * Trivially copyable and pointer-sized-plus-index — pass it by value.
 *
 * Opaque the same way @ref vertex_handle_t is, and for the same reason (ADR-0056): the pair it
 * carries is `graph_t`'s state, not the caller's. `graph_t` is the sole `friend` — the only code
 * that can build one from a vertex and a slot, and the only code that can read either back — so a
 * caller can neither reach the `vertex_t` behind a live subscription (whose slot mutators are only
 * valid under the graph's locks) nor forge a handle from an arbitrary pointer and index and hand
 * it to @ref graph_t::unsubscribe. A default-constructed handle names no subscription and
 * unsubscribes to a `NOT_FOUND` no-op; @ref operator== is the only observation a caller has.
 */
class subscription_t {
   public:
    /** @brief A handle naming no subscription — @ref graph_t::unsubscribe answers `NOT_FOUND`. */
    subscription_t() = default;

    /** @brief Two handles compare equal iff they name the same slot on the same producer vertex.
     *         (`!=` is synthesized.) */
    [[nodiscard]] friend bool operator==(const subscription_t& a,
                                         const subscription_t& b) noexcept {
        return a.vertex_ == b.vertex_ && a.slot_ == b.slot_;
    }

   private:
    friend class graph_t;  // sole constructor + the only code that reads the pair back.
    subscription_t(vertex_t* vertex, std::size_t slot) noexcept : vertex_(vertex), slot_(slot) {}
    vertex_t* vertex_ = nullptr; /**< @brief The producer vertex the edge lives on. */
    std::size_t slot_ = 0;       /**< @brief The `:subscribers[]` slot index. */
};

// Pass-by-value, as the doc comment above promises: privatizing the pair costs no wrapper.
static_assert(std::is_trivially_copyable_v<subscription_t>);

/**
 * @brief One node-scoped vertex reference — a slot index AND the generation stamping it
 *        (RFC-0024 §4.4 / §6.4).
 *
 * The pair is the unit a mint hands out, never two separately-read numbers: an index without
 * the generation that was current when it was read is not a reference to a vertex, it is a
 * reference to whatever the slot holds later. Retirement moves the generation and takes the
 * graph's map lock uniquely, so reading both under one hold is what makes the pair name a
 * single tenancy of the slot.
 */
struct vertex_slot_t {
    std::uint32_t index = 0;      /**< @brief Position in the node-scoped vertex index. */
    std::uint32_t generation = 0; /**< @brief The slot's retirement generation at that moment. */

    /** @brief Value equality — both fields, since either alone is not a reference. */
    [[nodiscard]] friend constexpr bool operator==(vertex_slot_t, vertex_slot_t) = default;
};

/**
 * @brief An operation's subject token — opaque bytes matched against ACE subjects (ADR-0018).
 */
using subject_token_t = std::vector<std::byte>;

/**
 * @brief The pluggable subject resolver (ADR-0018, #81): caller context → subject token.
 *
 * Maps an operation's caller context — this node's NAME for the inbound link a remote FWD
 * arrived on — to the subject token ACL evaluation matches against ACE subjects. The token
 * is identity-provenance: v1 typically returns the transport-authenticated peer id for a
 * link; a stronger (PKI) token slots in later without changing the ACL model.
 *
 * @warning The ERROR arm is a **deny**, not a fallback (#905). A resolver that cannot name
 *          the caller returns `std::unexpected(wire::err_t::ACCESS_DENIED)` and the
 *          operation fails `status_t::PERMISSION_DENIED` at every gate — READ, WRITE,
 *          SUBSCRIBE, CREATE, WRITE_ACL, READ_ACL, and remote-edge fan-in delivery. It is
 *          never invoked with an empty caller: the empty (local API) context is the
 *          trusted-by-convention channel and `graph_t::acl_allows` short-circuits it
 *          BEFORE the resolver, so a remote identity — which always carries a non-empty
 *          inbound link NAME — cannot reach the trusted arm. The predecessor of this type
 *          returned `std::optional`, whose `nullopt` meant "fully trusted": an
 *          unresolvable caller was granted everything, `WRITE_ACL` and `CREATE` included.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049). The predecessor was
 *       a `std::function` in a plain member that the ACL gate read on every gated read and
 *       write while a public setter could assign it — and assigning a `std::function`
 *       DESTROYS the old target, so a setter racing a gate freed the resolver's captured
 *       state while a reader was inside the call. A bare function pointer is publishable in
 *       one word, so the pair lives in a @ref tr::sink_slot_t and the gate reads a coherent
 *       snapshot for the same single load the null check already cost. Whatever state the
 *       resolver needs travels in @p ctx, which the caller owns and must keep alive across
 *       every possible gated operation.
 */
using subject_resolver_fn_t =
    std::expected<subject_token_t, wire::err_t> (*)(void* ctx, std::string_view caller);

/**
 * @brief One EXTERNAL mutation of a producer's `:subscribers[]` — what @ref
 *        graph_t::configure_subscription_observer reports.
 *
 * "External" is exactly the ADR-0018 caller context being NON-EMPTY: the op arrived through
 * `op_resolver_t` carrying an inbound link NAME. It is the same discriminator the SUBSCRIBE
 * gate already runs under, so an observer sees precisely the set of edges a remote peer
 * caused and never the ones the owner's own wiring code made. The local doors — both
 * `subscribe()` sugars, `unsubscribe()`, and a `:subscribers[]` field-write under the empty
 * context — are deliberately silent: the host that called them already knows.
 *
 * Both path fields are CANONICAL KEYS (concatenated NAME records — the `PATH` payload,
 * docs/reference/03), never a slash-spelled string: that is the form the graph addresses
 * by, and rendering one is the consumer's choice, not a cost the event imposes. Both are
 * BORROWED for the duration of the callback only — copy what outlives it.
 */
struct sub_event_t {
    /** @brief Which way the slot moved. */
    enum class kind_t : std::uint8_t {
        ADDED,  /**< @brief A slot was appended, or an empty slot filled by a `[N]` replace. */
        REMOVED /**< @brief An active slot was cleared, or displaced by a `[N]` replace. */
    };

    /** @brief Whether a slot gained a subscriber or lost one. */
    kind_t kind = kind_t::ADDED;
    /** @brief Canonical key of the PRODUCER — the vertex whose `:subscribers[]` changed. */
    wire::key_view_t producer;
    /**
     * @brief Canonical key decoded from the `SUBSCRIBER`'s `PATH` child — WHAT the record
     *        says, verbatim.
     *
     * EMPTY when the record carries no well-formed `PATH` at all (a bare remote subscriber,
     * whose consumer is named only by its return route over @ref link).
     *
     * @warning Read it as the consumer's SPELLING, not as a local vertex. On a wire subscribe
     *          that binds a REMOTE subscriber this `PATH` names the consumer at ITS OWN root
     *          and resolves to nothing here — `subscribe_wire` deliberately drops it as a
     *          re-dispatch target and delivers over the return route instead (RFC-0004 §D).
     *          On a local-target append it IS a key in this graph. The two are not
     *          distinguishable from the event alone; @ref link tells the observer which
     *          transport the op came from, and the app's own wiring says the rest.
     */
    wire::key_view_t target;
    /** @brief This node's NAME for the transport link the op arrived on. Never empty. */
    std::string_view link;
    /** @brief The `:subscribers[]` slot index the event concerns (RFC-0009 §D.2 stable). */
    std::size_t slot = 0;
};

/**
 * @brief The app-installable external-subscription observer.
 *
 * @warning Runs SYNCHRONOUSLY on the resolver's thread, inside the operation it reports, and
 *          the reply is not assembled until it returns — so it must be cheap and
 *          non-blocking, and it MUST NOT re-enter `graph_t`. It is called outside every
 *          graph lock (the admission door has already released the vertex stripe lock and
 *          the map lock), so a re-entrant call does not self-deadlock; it is refused on the
 *          simpler ground that an observer which mutates the graph while a `:subscribers[]`
 *          write is mid-flight makes the event stream depend on its own side effects.
 *          Deferral — queueing the event and acting on it from the app's own task — is the
 *          APP's job, exactly as it is for @ref graph_t::configure_remote_delivery_sink.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049) — see
 *       `subject_resolver_fn_t` for why. @p ctx is caller-owned and must outlive every
 *       subscription mutation the graph can still report.
 */
using sub_observer_fn_t = void (*)(void* ctx, const sub_event_t& event);

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
     * @brief Construct a graph drawing its per-write control-block allocations from
     *        @p mr (ADR-0039 §1, #361 §5) and its write-path value byte-buffers from
     *        @p value_backend (ADR-0060).
     *
     * @p mr allocates the LKV control block + `rope_t` wrapper object; @p
     * value_backend is the L0 byte-buffer seam the write-path copy-store draws its
     * owned value @ref view::segment_t from — the single flatten of a branch or field
     * write (`graph.cpp` sites 825, 1017). A bounded node points BOTH at one static
     * slab ("one slab, whole stack"); a host passes nothing and gets the standard
     * heap for each (zero churn, behaviour byte-identical).
     *
     * The seam's scope is PAYLOAD bytes, which includes READ-path framing and not only
     * the write-path copy-store (#831): BOTH folded READs frame their exactly-sized POINT
     * headers from it — one per subtree node in the composed-root fold, and one per
     * registered child plus the outer listing header in the `":children"` fold the wire
     * field READ routes to. These are payload bytes whose length field wraps the stored
     * TLV and the name records below it, as distinct from
     * the route-byte-sized reply-egress seam of ADR-0074. Both counts are peer-influenced, so an
     * injector sizing a bounded slab must budget for them; the size classes are the
     * host's composition problem (ADR-0060 §3 keeps the graph size-agnostic).
     *
     * An injected @p value_backend MUST be thread-safe (ADR-0060 §2): a value @ref
     * view::segment_t self-routes its reclaim on whatever thread drops the last ref —
     * typically a reader/subscriber, concurrent with a writer's `alloc` — so
     * sharding it per lock-stripe removes no race. The default `heap_backend()`
     * already is thread-safe; a `pool_t` must be composed with the target's
     * arch-selected synchronisation. On exhaustion `value_backend` returns `nullptr`
     * (the BACKPRESSURE signal), and the write rejects rather than silently falling
     * back to the heap (§3). @p mr and @p value_backend must both outlive the graph
     * and every value handle obtained from it.
     *
     * @param ctl The #551 nothrow seam every FAILABLE allocation draws from — the ones a
     *        PEER can provoke ("failable", not "control-plane": CONTEXT.md binds that
     *        phrase to the `:` field-write plane): vertex registration first, then the
     *        `route_handle` label tables, `tlv_arena` nodes, `fwd_router` iov and
     *        `can_reassembly` maps as each migrates. On exhaustion it returns
     *        `nullptr` and the operation answers BACKPRESSURE, so a peer's CREATE
     *        frame can no longer reboot a `-fno-exceptions` node. Deliberately a
     *        DIFFERENT C++ type from @p mr so the two contracts (must-not-be-null
     *        vs may-be-null) cannot be transposed by a one-token edit, and so
     *        retiring @p mr later is a compile error rather than a silent rebind.
     *        Appended, not prepended, so every existing `graph_t{&mr}` call site
     *        compiles unchanged. Must outlive the graph, like the other two.
     */
    explicit graph_t(std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                     mem::mem_backend_t* value_backend = &mem::heap_backend(),
                     mem::block_source_t* ctl = &mem::heap_source());
    graph_t(const graph_t&) = delete;
    graph_t& operator=(const graph_t&) = delete;

    /**
     * @brief The injected #551 nothrow failable-block seam (@ref tr::mem::block_source_t).
     *
     * Exposed so a host can name it in a memory census and so the wiring is
     * observable without reaching into the graph's state. Callers inside the
     * library draw from `ctl_` directly.
     */
    [[nodiscard]] mem::block_source_t& control_source() const noexcept { return *ctl_; }

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
                                                  handlers_t handlers = {});

    /**
     * @brief Register a vertex at @p path — FALLIBLE (the runtime-path form of
     *        @ref register_vertex).
     * @return The pinned @ref vertex_handle_t, or `PATH_IN_USE` if the path is already
     *         registered.
     */
    [[nodiscard]] result_t<vertex_handle_t> try_register_vertex(const path_t& path, role_t role,
                                                                handlers_t handlers = {});

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
                                                                handlers_t handlers = {});

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
     * owner-side storage declarations, and delivery mode, so a later write-creates revive
     * of the same address
     * inherits **nothing** of the retired owner — in particular the revived path inherits
     * its live ancestor's ACL policy, never the retired one's (the §Discussion-7 ruling:
     * an ACL does not survive churn). `write_seq_` survives (monotonic per address).
     *
     * Delivers nothing and wakes no `await` (§B.5). Idempotent (§B.4): retiring an
     * already-retired or unregistered vertex succeeds and does nothing. The root cannot be
     * retired. There is **no wire operation** that reaches here — a peer goes through the
     * device's own logic (§A.1 / §A.1.1), which is what calls this.
     */
    [[nodiscard]] result_t<void> retire(vertex_handle_t vh);

    /**
     * @brief @p vh's retirement generation — the stamp a cached resolution carries (ADR-0062).
     *
     * A `vertex_handle_t` never dangles (the vertex map is pinned and insert-only), but
     * @ref retire re-virginizes the object in place. A holder that caches a resolved handle
     * — a route-handle terminus binding, say — records this alongside it and re-reads it
     * before use: a mismatch means the path was retired (and possibly re-created for a
     * DIFFERENT owner) since the resolution, so the cached answer must be discarded rather
     * than delivered into whatever now occupies that path.
     *
     * Lock-free; the counter is bumped under retirement's own ordering. Callers must NOT
     * cache an authorization decision this way — a generation match says the vertex is the
     * same one, never that the caller may still act on it (ACL stays per-operation).
     */
    [[nodiscard]] std::uint32_t retire_generation(vertex_handle_t vh) const noexcept;

    /**
     * @brief This vertex's OWN active subscriber-slot count (#635) — how many slots a
     *        delivery here would feed, for sizing and observability.
     *
     * @warning This is NOT the "is anyone listening" question, on two counts, and a
     *          producer must not gate a publish on it — use @ref has_subscribers.
     *          It omits subtree subscribers, who subscribe on a strict ANCESTOR and are
     *          counted by `listeners_above` rather than here (RFC-0005), so a zero here
     *          says nothing about them. And it is the relaxed load, which
     *          @ref vertex_t::own_subs_ordered documents as unfit for a skip decision.
     *
     * Relaxed by design: this answers "how much work would a delivery be", the use
     * @ref vertex_t::own_subs is specified for. A racing subscribe is observed by the next
     * read at worst, which is what a sizing hint needs.
     */
    [[nodiscard]] std::uint32_t own_subs(vertex_handle_t vh) const noexcept {
        return vh.get()->own_subs();
    }

    /**
     * @brief Would a **delivery** at @p vh reach any subscriber — its own, OR a subtree
     *        subscriber on a strict ancestor (RFC-0005)?
     *
     * The gate for a demand-driven producer that wants to skip *delivery work*. It joins the
     * two gates `deliver_vertex`, the per-vertex delivery unit, applies — `fan_out`'s own
     * self-gate on the own count, then the `listeners_above` gate `deliver_vertex` holds over
     * `bubble_up` — so a producer that skips a `deliver_vertex` on `false` skips exactly what
     * that call would have found no receiver for. (A decomposing BRANCH write is not one
     * `deliver_vertex`: it fans out at each descendant landing site under that site's own
     * gate, which this predicate does not answer for.) Gating on the own-slot count alone
     * silently drops every subtree subscriber, which is why @ref own_subs carries a warning
     * against it. (`mark_pending`, the deferred half, gates on `delivery_mode` first and so
     * asks a third question this predicate deliberately does not.)
     *
     * @warning **Subscribers are not the only consumers.** `read` pollers and threads blocked
     *          in @ref await are invisible here — this counts subscription edges only, which
     *          ADR-0006 makes a field-write to `:subscribers[]` rather than one of its three
     *          verbs. A producer that skips its *delivery* on `false` is fine; one that also
     *          skips the VALUE STORE starves every awaiter (no `write_seq_` bump to wake
     *          them) and freezes the LKV for every reader.
     *
     * @warning **A skip here has no durability-latch backstop.** ADR-0049's latch belongs to
     *          @ref vertex_t::own_subs_ordered's fan-out skip, whose protocol is *store the
     *          LKV, THEN load the count*; a producer that skips on this predicate never
     *          reaches the store, so there is no new value to latch. What ordering this
     *          predicate does give is just its two loads' — the `seq_cst`
     *          @ref vertex_t::own_subs_ordered and the relaxed `listeners_above` — making it
     *          exactly as ordered as `deliver_vertex`'s own two gates and no more. The
     *          `seq_cst` half's argument is documented there; it is not restated here.
     *
     * @warning **The ancestor half can be one subscribe behind, with no bound but the
     *          platform's.** `listeners_above` is a relaxed load, so a `false` here may miss a
     *          subtree subscribe that has already COMPLETED on another thread — latch taken,
     *          counter bumped — and nothing synchronizes when this reader catches up. That
     *          staleness is deliberate and ruled on measurement (#854, REFUTED — the `seq_cst`
     *          candidate doubled the idle write's fence count on rv32 and bought nothing): per
     *          the #555 standard, the outcome a stale-`false` skip produces — the racing
     *          publish reaching no subtree subscriber — is indistinguishable from the write
     *          linearizing BEFORE the subscribe, and the subscriber's ADR-0049 latch cannot
     *          contradict that ordering, because the latch snapshots the SUBSCRIBED ancestor's
     *          own LKV (@ref vertex_t::add_edge), which never holds a descendant's value.
     *          There is no forbidden observation for an ordered load to exclude, so the
     *          ordered load does not exist.
     *
     * @note Swapping the `seq_cst` own half for the relaxed @ref vertex_t::own_subs leaves
     *       the whole suite green, so its presence here rests on that argument, not coverage.
     */
    [[nodiscard]] bool has_subscribers(vertex_handle_t vh) const noexcept {
        const vertex_t* const v = vh.get();
        return v->own_subs_ordered() != 0 || v->listeners_above() != 0;
    }

    /**
     * @brief Slots in the node-scoped vertex index — the cardinality a bound-path element's
     *        index is bounds-checked against (RFC-0024 §6.4).
     *
     * One slot per `vertex_t` ever allocated in this graph, in allocation order, slot 0 being
     * the structural root. The index is **append-only** because registration already is
     * ("vertices are added, never erased"), so a slot handed out once names the same
     * allocation for the graph's lifetime and there is no new invalidation event to observe.
     *
     * Node-local and unobservable on the wire: a peer learns another node's cardinality only
     * by being handed an element that came from it, and an element is meaningless anywhere
     * but on the host that minted it.
     */
    [[nodiscard]] std::size_t vertex_slot_count() const noexcept;

    /**
     * @brief Register — or REVIVE — a session **identity anchor**: a vertex that exists to
     *        be REFERENCED and never to be ADDRESSED (#1223 step 2).
     *
     * ADR-0044's 2026-08-13 amendment scopes §Decision 1 to announce-census peers and lets
     * an **accepted** ws/tcp session hold a vertex, so that the session's death is a RETIRE
     * and a route naming it fails the RFC-0024 §5.1 generation check. This is the seam that
     * gives it one. @p id is the session's node-scoped identity string — the router composes
     * it from the mount's qualified name and the peer's slot name, so the SAME slot always
     * asks for the SAME anchor.
     *
     * **An anchor is not part of the addressable tree, deliberately.** It hangs off a private
     * structural root that `root_` cannot reach, so:
     *   - `find`, `read`, every path descent and every `:children[]` listing are byte-for-byte
     *     unchanged — an anchor is invisible to all of them. That is what keeps
     *     `bus_link_t::enumerate_peers` the ONE source of truth for a bus vertex's synthesized
     *     members (ADR-0044 §Decision 1, unamended in this respect), instead of a second one.
     *   - nothing below a bus mount becomes locally resolvable, so RFC-0020 §3's MUST ("a node
     *     MUST NOT resolve the residual against its local graph") keeps the premise it was
     *     argued on. An anchor cannot be the shadow vertex that MUST is about, because no
     *     spelling of any `dst` reaches it.
     * What an anchor DOES have is the only thing it is for: a slot in the pinned, insert-only
     * vertex map, hence a `(index, generation)` an RFC-0024 element can name.
     *
     * **Revive is in place.** The anchor for a given @p id is allocated ONCE and re-`fill`ed
     * afterwards, exactly as a retired addressable vertex is revived by a second registration
     * at its path — so a recycled `p<slot>` returns the SAME `vertex_t` in the SAME slot with
     * only the saturating retire generation bumped (RFC-0024 §4.4 rule 3). Anchor count is
     * therefore bounded by the listener's `max_peers`, not by session churn, which is the
     * measurement the ADR amendment rests on.
     *
     * Retire an anchor through the ordinary @ref retire — it is an ordinary vertex in every
     * respect the mint, the deref and retirement care about.
     *
     * @retval status_t::PATH_IN_USE @p id already names a LIVE anchor (a duplicate arrival
     *         notification, or an id collision). The caller keeps the existing anchor.
     */
    [[nodiscard]] result_t<vertex_handle_t> register_session_anchor(std::string_view id);

    /** @brief The live anchor for @p id, or `std::nullopt` when none is registered (it was
     *         never created, or it has been retired). Never descends the addressable tree. */
    [[nodiscard]] std::optional<vertex_handle_t> find_session_anchor(std::string_view id) const;

    /**
     * @brief How many anchor `vertex_t`s this graph has ever ALLOCATED — live or retired.
     *
     * The bounded-across-churn number, exposed so a test can assert it rather than infer it:
     * it counts allocations, not registrations, so a revive must leave it unchanged.
     */
    [[nodiscard]] std::size_t session_anchor_slots() const noexcept;

    /**
     * @brief This node's own reference to @p vh — the MINT side of a bound-path element
     *        (RFC-0024 §6.4, §7).
     *
     * Returns the index **and** the generation that stamps it, because the two are one fact:
     * read separately they can straddle a `retire`, and the pair would then name the
     * successor tenant's vertex while the caller believes it bound the one its operation
     * reached. Both fields are read under a single `map_mutex_` hold, which retirement takes
     * uniquely, so the pair is always a consistent snapshot.
     *
     * @retval std::nullopt @p vh's generation has SATURATED (`kGenerationSaturated`), so
     *         the vertex is permanently unbindable and the caller stays on the canonical
     *         form (RFC-0024 §4.4 rule 3) — or, defensively, @p vh is not in this graph's
     *         index at all.
     *
     * @warning This is a **control-plane** call and is priced as one: it finds @p vh by
     *          scanning the slot index, because the reverse direction is deliberately not
     *          memoized. A per-vertex index field costs 4 bytes on rv32, where
     *          `sizeof(vertex_t)` sits at its ceiling with zero headroom
     *          (`config_t::kMaxVertexBytes32`), and a pointer→index side map costs strictly
     *          more than the 4 B/vertex RFC-0024 §6.4 priced. A mint happens once per
     *          binding, on a reply already being assembled; the hot path — @ref
     *          deref_vertex_slot — pays a bounds check and one compare and never comes here.
     */
    [[nodiscard]] std::optional<vertex_slot_t> vertex_slot(vertex_handle_t vh) const noexcept;

    /**
     * @brief Dereference a bound-path element — the §5.1 check, and the whole of it.
     *
     * Bounds-checks @p index against @ref vertex_slot_count, refuses a SATURATED
     * @p generation outright, and compares the rest against the slot's
     * @ref retire_generation. The vertex map is pinned, pointer-stable and insert-only, so
     * an in-range index always names a live allocation and the deref itself cannot fault.
     *
     * A generation only ever moves forward, so a stale element can only ever compare lower
     * and never becomes valid again by waiting — **except at the ceiling**, where the
     * counter stops. There, and only there, "moves forward" stops being a guard: a
     * `kGenerationSaturated` element would match the slot for the rest of the node's life,
     * across every subsequent retire and revive, so staleness detection would be dead for
     * that slot and the #603 misroute class the saturation rule exists to close would be
     * open again. The mint refuses to issue such an element; this refuses to honour one,
     * which is what makes "permanently unbindable" (RFC-0024 §4.4 rule 3) a property of the
     * vertex rather than of one code path's good manners.
     *
     * @retval std::nullopt Out of range, saturated, or the generation does not match. The
     *         caller MUST then drop — never forward, never apply, never repair
     *         (RFC-0024 §5.3).
     *
     * @warning A match authorizes **nothing**. It says the vertex is the same one, never
     *          that the caller may still act on it: every bound-form operation re-evaluates
     *          `acl_allows` at the dereferenced vertex for its own right, exactly as the
     *          canonical form does (RFC-0024 §6.2). The graph's own data ops do that
     *          themselves, which is why the two spellings are equivalent by construction.
     */
    [[nodiscard]] std::optional<vertex_handle_t> deref_vertex_slot(
        std::uint32_t index, std::uint32_t generation) const noexcept;

    /**
     * @brief The element a mint would issue for the slot at @p index — the FORWARDER's mint
     *        (RFC-0024 §7.1 step 2), in O(1).
     *
     * The terminus mints for a vertex it just resolved, so it has a handle and can afford
     * @ref vertex_slot's scan. A forwarder mints for the connection vertex of the link a
     * reply arrived on — a vertex whose index it recorded once, at registration — so all it
     * needs is that index's CURRENT generation, and paying a scan of the whole index per
     * forwarded reply to re-derive an index it already holds would be the wrong shape at the
     * wrong place. This is the same read the other way round: index in, generation out.
     *
     * @retval std::nullopt @p index is out of range, the slot's generation has SATURATED — a
     *         permanently unbindable vertex (RFC-0024 §4.4 rule 3) — or the slot holds a
     *         retired/never-registered PLACEHOLDER, which `deref_vertex_slot` refuses on the
     *         honouring side and which is therefore refused here too: otherwise the window
     *         between a retire and its revival mints an element valid against the SUCCESSOR
     *         tenancy. A forwarder that cannot mint STRIPS the mint answer (§7.1 erratum 1)
     *         and the origin stays canonical.
     */
    [[nodiscard]] std::optional<vertex_slot_t> vertex_slot_at(std::uint32_t index) const noexcept;

    /**
     * @brief Evaluate the ACL at @p v for @p caller and @p right — the §6.2 check, exposed.
     *
     * The same predicate every data op already runs before it acts, published for the ONE
     * caller that reaches a vertex without performing a data op on it: the bound-path
     * forwarder, whose element dereferences to a **connection** vertex it will egress
     * through rather than read or write (RFC-0024 §6.2 — "every operation arriving on a bound
     * path MUST evaluate `acl_allows` at the dereferenced vertex, for the operation's own
     * right"). Nothing is cached: an `:acl` write marks the subtree dirty and the next call
     * rebuilds, so a revoked right takes effect on the very next frame over an already-minted
     * binding.
     *
     * @param v      The vertex to evaluate at.
     * @param caller The subject context — a transport link name; empty is the trusted local
     *               caller, which is allowed everything (the shipped convention).
     * @param right  The right the operation needs.
     */
    [[nodiscard]] bool allows(vertex_handle_t v, std::string_view caller, acl_right_t right) const;

    /**
     * @brief Free every value seam @ref retire parked — the EXPLICIT collector (#576).
     *
     * @ref retire detaches a vertex's value seam and **parks** it: the seam is read
     * lock-free, so the retiring thread cannot free the block a concurrent reader may
     * still be dereferencing. Parking alone has no other end, so a node that retires
     * seam-bearing vertices repeatedly grows the park forever. This is that other end, and
     * it is the embedder's call, not the library's.
     *
     * **Which vertices park — handler PRESENCE, never role.** `vertex_t::adopt_identity`
     * allocates the `value_handlers_t` iff at least one of `on_read`, `on_write`,
     * `on_children` was installed at registration; `role_t` is never consulted. So a
     * `role_t::STORED_VALUE` vertex registered with an `on_children` parks one seam on
     * retirement, and a `role_t::HANDLER` registered with an empty @ref handlers_t parks
     * nothing. Scoping a quiescent point by role excludes exactly the production case
     * below.
     *
     * **The one peer-driven append site is conditional.**
     * `tr::net::transport_vertex_t::remove_connection` retires the `/net/<module>/<name>`
     * identity vertex, which is registered `role_t::STORED_VALUE` — and it bears a seam
     * only when its link exposes a bus facet (`transport_t::bus() != nullptr`): the CAN
     * binding, and a tcp/ws server wired `peer_named = true`, get an `on_children` that
     * synthesizes the live peer listing (ADR-0044). A point-to-point deployment — every
     * dial link, UDP, loopback, a default-wired server — parks **nothing** on teardown and
     * needs no quiescent point at all. A bus node parks one `value_handlers_t` (~96 B of
     * `std::function`, plus each callback's captures) per teardown; that node is the one
     * this method exists for.
     *
     * @warning **The caller MUST call this from a point where no lock-free reader holds a
     *          value seam.** The library cannot know that moment — a reader holds the raw
     *          seam pointer across the user callback it invokes — so naming it is an API
     *          obligation this method hands to the embedder. On a single-threaded node any
     *          point between operations qualifies. On a threaded node, a point where the
     *          graph is quiescent for reads does: after the transport plane's receive
     *          threads are joined or paused, or on the one thread that runs every graph
     *          operation. The hazard is NOT limited to a thread that started on an
     *          already-retired vertex: `read` / `write` / `:children[]` load the seam
     *          pointer ONCE (deliberately — a second load could see a concurrent retire's
     *          null), so a thread that entered while the vertex was still **LIVE** holds
     *          that raw pointer across the whole user callback, and a retire landing
     *          mid-callback moves the block it is using into the park. Collecting while
     *          any such call is in flight — retired first or not — is a use-after-free.
     *
     * The free runs on the CALLER's thread and OUTSIDE every graph lock: the parked list is
     * swapped into a local under the map lock, and the local destructs after the lock is
     * released. So a seam callback's destructor may re-enter the graph (drop a handle,
     * `find` a path, retire something else) without deadlocking, and an arbitrarily slow
     * destructor blocks no reader or writer.
     *
     * Idempotent, and a no-op when nothing is parked. Not itself a reader-safety
     * mechanism: it neither waits for nor detects readers. An embedder that never calls it
     * keeps the pre-#576 behaviour — the park grows without bound — which @ref
     * parked_seam_count makes observable.
     *
     * @note Whatever is still parked when the graph is destroyed is freed by the graph's
     *       own teardown — a backstop against unbounded growth, NOT a substitute for this
     *       call. `retired_seams_` is declared before `map_mutex_` and `root_`, so it
     *       destructs **last**, after the vertex tree and the map lock are already gone: a
     *       seam callback whose destructor re-enters the graph re-enters a half-destroyed
     *       object and crashes. Such an owner is safe HERE and only here — it must be
     *       collected explicitly, never left to teardown.
     */
    void collect();

    /**
     * @brief How many retired value seams are currently parked, awaiting @ref collect.
     *
     * The observability half of the collector: an embedder that never calls @ref collect
     * has a number it can watch (a health field, an assert in a soak test) instead of a
     * silent, peer-driven leak. Grows by one per retired vertex that BORE a value seam —
     * i.e. one that had any of `on_read` / `on_write` / `on_children` installed at
     * registration, whatever its `role_t` — and drops to zero on @ref collect. A
     * retired vertex with no value seam parks nothing, including a `role_t::HANDLER` one
     * registered with an empty @ref handlers_t. On the transport plane that means one per
     * `/net/<module>/<name>` identity vertex whose link exposes a bus facet (CAN, or a
     * tcp/ws server wired `peer_named = true`) and **zero** for every point-to-point
     * connection — so on a default deployment this legitimately never leaves 0.
     */
    [[nodiscard]] std::size_t parked_seam_count() const;

    /**
     * @brief Evict every subscriber edge a departed link left behind — the graph half
     *        of link-teardown eviction (RFC-0009 §D, extended to peer departure).
     *
     * Walks the whole graph and deactivates + RECLAIMS each active subscriber edge
     * whose stored link NAME equals @p link_name (the NAME this node addressed the
     * link by — a bus peer's tag, or a point-to-point child's registered NAME),
     * unwinding the RFC-0005 listener bookkeeping for each. Local edges and edges of
     * other links are untouched; slot indices of surviving edges never renumber
     * (§D.2), and the freed slots are reused by later appends (@ref vertex_t's
     * add_edge reuse) — so a redialing peer's re-subscriptions reoccupy the memory
     * its dead session held instead of growing every vertex's edge list forever.
     *
     * A local, host-facing API in the §A.1 sense: no wire operation reaches here —
     * the transport plane calls it when it LEARNS a link died (`fwd_router_t::
     * link_down`, the link-departure hook), exactly as the owner's own logic might.
     * Concurrency: the vertex set is snapshotted under a shared `map_mutex_` hold,
     * then each vertex is evicted under its own stripe lock inside a fresh shared
     * hold (never across vertices), so concurrent writes/deliveries interleave
     * freely; an in-flight delivery keeps its route alive by refcount clone
     * (ADR-0041 §2). Safe to call for a link that never subscribed (a no-op).
     *
     * An EMPTY @p link_name matches nothing and returns 0. This entry point reports a
     * COUNT and has no error channel, so a nameless link is a no-op rather than a
     * status: a link with no name never subscribed anything. It is a rule, not a
     * coincidence of the comparison — a LOCAL admission stores the empty caller
     * context, so before #1056 an empty key compared equal to every local edge that
     * carried a cold half (the `delivery_compact` opt-in) and reclaimed it graph-wide.
     * @param link_name This node's NAME for the departed link; empty ⇒ no-op, 0.
     * @return The number of edges evicted, summed over the graph.
     */
    std::size_t evict_link_edges(std::string_view link_name);

    /**
     * @brief How many vertices a @ref evict_link_edges for @p link_name would EXAMINE — the
     *        departure's cost, observable (#1071).
     *
     * A diagnostic, and the instrument the #1071 acceptance test asserts on: before the
     * per-link index this number was "every vertex in the graph holding any subscriber
     * edge", so one peer's hangup was priced by every OTHER peer's subscriptions. It is now
     * the count of vertices that peer itself ever subscribed on.
     *
     * Reports the INDEX's size, not a live edge count, and the two differ by design: the
     * index is a superset that keeps a vertex after an individual unsubscribe (see
     * `link_index_`), so this can exceed the number of edges an eviction would actually
     * reclaim. It is an upper bound on work, which is exactly what the scaling property is
     * about — never read it as "how many edges this link has".
     * @param link_name This node's NAME for the link; empty ⇒ 0 (a local edge is not
     *                  reachable by link teardown).
     * @return The number of candidate vertices, i.e. the departure's bounded cost.
     */
    [[nodiscard]] std::size_t link_edge_candidates(std::string_view link_name) const;

    /**
     * @brief Evict the remote subscriber edge(s) whose delivery link AND stored return
     *        route both match — the refused-route reclaim (#1223 step 5).
     *
     * The narrow sibling of @ref evict_link_edges, fired by the transport plane when a
     * delivery it emitted draws back an addressed `tr::path::invalid` refusal (the RFC-0020
     * bus-residual reject — the one wire observation a producer gets that a stored route's
     * terminal session departed). Where link teardown reclaims a whole link's edges, this
     * reclaims exactly the edge(s) that delivered along @p route_wire over @p link_name:
     * both keys are required, and the route compare is BYTE-equal on the stored PATH TLV
     * (see `vertex_t::evict_route_edges`). Same two-phase locking, same RFC-0005 unwind,
     * same no-error-channel contract as link teardown; an empty key matches nothing.
     *
     * RFC-0009 §D.4 is NOT contradicted: that clause keeps an edge whose *target vertex*
     * retired, on the stated premise that "a write to a retired path is not an error the
     * producer observes". A refused ROUTE is precisely the case where the producer now DOES
     * observe an error — RFC-0020 (which postdates §D.4) made the observation normative,
     * and this reclaim acts only on it.
     *
     * @param link_name  This node's NAME for the link the refusal arrived on.
     * @param route_wire The refused route — whole TLV bytes echoed by the rejecting hop: a
     *                   canonical PATH, or (RFC-0024 §7.1 amendment 1) the bound `PATH_REF`
     *                   a reverse-list delivery was refused as. This door classifies the
     *                   type byte; the per-vertex half stays wire-type-agnostic.
     * @return The number of edges evicted, summed over the graph.
     */
    std::size_t evict_route_edges(std::string_view link_name,
                                  std::span<const std::byte> route_wire);

    /**
     * @brief The (mount, peer) a SESSION ANCHOR names, or nullopt for every ordinary vertex —
     *        the reverse-list delivery's egress question (RFC-0024 §7.1 amendment 1, #1223).
     *
     * A bound delivery's LAST element dereferences to the accepted session's identity vertex
     * (the #1254 anchor); the hop that consumes it must egress to the SESSION, and this is
     * where it learns which one. Classification is by the anchor's own key shape — the id is
     * `:<mount>/<peer>`, and both `:` and `/` are characters `path::valid_segment` forbids,
     * so no addressable vertex's key can ever satisfy it (the same argument that makes the
     * anchor unspellable makes this test unforgeable). The views are BORROWED from the
     * vertex's immutable key record and stay valid for the graph's life (vertices are never
     * freed).
     */
    struct session_anchor_route_t {
        std::string_view mount; /**< @brief The bus child's registered NAME. */
        std::string_view peer;  /**< @brief The accepted session's routable name. */
    };
    /** @brief Classify @p vh per the block above: the anchor's (mount, peer), or nullopt
     *         for every ordinary vertex. Lock-free — the key record is immutable. */
    [[nodiscard]] std::optional<session_anchor_route_t> session_anchor_route(
        vertex_handle_t vh) const noexcept;

    /**
     * @brief Visit every REGISTERED vertex once, in ascending canonical-key BYTE order —
     *        the graph's enumeration surface (a census, a directory listing, a paginated
     *        `/system/…` projection).
     *
     * `fn` is invoked as `fn(wire::key_view_t key, vertex_handle_t vh)`. @p key is the
     * vertex's full canonical key (concatenated NAME records, the `PATH` payload) rendered
     * on demand — ADR-0057 stores one segment per node, so no such key exists until this
     * asks for it — and is BORROWED for the duration of that one call. Placeholders (the
     * unregistered intermediates a deep `register_vertex` creates) are SKIPPED: they are
     * addressing scaffolding, not vertices an owner declared, and `find` does not answer
     * for them either.
     *
     * **SORTED, and there is no unsorted twin**, deliberately. The tree walk's natural order
     * is `for_each_descendant`'s, which no caller should encode a dependency on; a consumer
     * paginating this surface — "give me vertices 40..60" across two operations — needs the
     * order to be the SAME both times whenever the graph did not change, and byte order over
     * canonical keys is the only order this container can promise that of. The sort is not
     * what costs: every visit needs @p key, and rendering the keys is already `O(n)`
     * allocations, so ordering them is a comparison pass on top of work the unsorted form
     * would have done anyway. Offering both would buy nothing and invite the wrong one.
     *
     * The order is the SAME one the RFC-0008 sweep sets are kept in (`pending_` /
     * `unconditional_`, byte-keyed `std::set`s), and it earns its keep the same way: the
     * length-prefixed NAME framing makes a parent's key a byte-prefix of every descendant's,
     * so a parent always precedes its subtree and that subtree is a CONTIGUOUS run. The
     * result therefore reads as a stable pre-order tree listing.
     *
     * @note It is byte order over the KEY, not alphabetical order over the spelled path. A
     *       NAME record is `02 00 <u16 len> <text>`, so siblings sort by name LENGTH first
     *       and only then by text (`/zone` before `/sensor` before `/actuator`). A consumer
     *       that wants alphabetical DISPLAY order sorts what it collected; what this promises
     *       is stability and subtree contiguity.
     *
     * @warning **CONTROL-PLANE ONLY** and priced as such: it allocates one owned key per
     *          registered vertex plus the snapshot vector, then sorts. Do not put this on a
     *          delivery or write path.
     *
     * Concurrency: the {key, vertex} snapshot is taken under ONE shared `map_mutex_` hold and
     * `fn` runs OUTSIDE it — the same two-phase discipline @ref evict_link_edges and the
     * fan-out sweep use. So `fn` MAY re-enter the graph (read a value, register a vertex,
     * retire something) without self-deadlocking. What it gets in exchange is a SNAPSHOT:
     * a vertex registered after the hold is not visited, one retired during the walk is
     * still visited (handles stay valid — vertices are pointer-stable and never freed,
     * ADR-0057), and a caller that must distinguish those re-reads under its own lock.
     * Vertices registered BEFORE the hold are all visited.
     */
    template <typename Fn>
    void for_each_vertex(Fn&& fn) const {
        std::vector<std::pair<std::vector<std::byte>, vertex_t*>> snap;
        {
            const std::shared_lock lock(map_mutex_);
            vertex_t* const r = root_.get();
            const auto take = [&snap](vertex_t& c) {
                if (c.registered()) snap.emplace_back(build_key(&c), &c);
            };
            take(*r);
            r->for_each_descendant(take);
        }
        std::sort(snap.begin(), snap.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& e : snap) fn(wire::key_view_t{e.first}, vertex_handle_t{e.second});
    }

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
     * (the ENOTTY of an unsupported creation). The built-in `stored_value` type is
     * registered by the constructor.
     *
     * CONFIGURATION, like the three `configure_*` sinks: populate the catalog at setup,
     * before frames flow. Unlike them the catalog is a `std::map`, so #1049's `{fn, ctx}`
     * publication does not reach it — a concurrent insert rebalances a tree the in-band
     * creation path may be walking. Registration and lookup therefore take a lock, which
     * costs nothing: both are control-plane cold (one map lookup per created vertex) and
     * neither is on a read, write or dispatch path. Violating the setup-only contract is
     * consequently slow rather than corrupting.
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
     * A vertex with ≥ 1 registered child serves the COMPOSED BRANCH READ instead — the
     * folded POINT tree of @ref read_subtree_folded (per-node stored TLVs verbatim,
     * READ-denied subtrees pruned): a view over the existing last-known-value ropes, not
     * a copy. Leaf reads are byte-identical to the pre-composed-read behavior, and a
     * HANDLER target's `on_read` seam keeps precedence over the composed read.
     */
    [[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, std::string_view caller = {}) const;
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
     * @brief Declare how many entries @p v's STREAM ring retains (RFC-0022 §3.C).
     *
     * The ring depth is **not** protocol QoS: it encodes what the APPLICATION wants kept,
     * and only the application can supply it. So it is an owner-side wiring call in the
     * shape of @ref set_delivery_mode and @ref set_app_fields — a declaration the owner
     * makes host-side after registration — and it has **no wire surface at all**: no peer
     * can read it and none can write it. Callable at any time; the next append trims to
     * the new depth. @p keep of 0 behaves as 1 (the ring always keeps the last value).
     *
     * Meaningful on the STREAM role, which is the only role that appends a ring; setting
     * it on another role stores the number and changes nothing. Costs a STREAM vertex zero
     * additional bytes — a STREAM identity already allocates the extension block.
     */
    void set_history_depth(vertex_handle_t v, std::uint32_t keep);
    /**
     * @brief Declare @p v's RFC-0022 §3.D pin amplification ratio `K` (ADR-0042 §3);
     *        @ref tr::graph::kPinNever (0, the default) disables pinning on this vertex.
     *
     * @p k is a RATIO, not a byte count. A view-delivered WRITE is stored as a refcounted
     * SUBVIEW of the inbound frame — no allocation, no copy — iff
     * `payload_bytes * k >= segment_bytes` and the payload is trailer-less; otherwise it
     * takes the one-copy trailer-sliced store. Pinning holds the WHOLE inbound segment for
     * the value's lifetime, so it buys latency and pays in RAM bounded at `(k-1)x` the
     * payload; that trade is a deployment call, which is why this is an owner-side
     * declaration and not, since RFC-0022 §3.B, a remotely writable knob. Nothing is
     * inherited (§3.F).
     *
     * @note This is a per-vertex OVERRIDE of `config_t::kPinPayloadRatio`, which Amendment 2
     *       fixes at the sentinel on both targets. It exists so §6-style measurement arms
     *       rotate inside one process — measuring them as separate binaries is what produced
     *       a 2.8x swing on identical code. Setting it IS the opt-in for that vertex; the
     *       override's existence changes nothing shipped, since both defaults are the sentinel.
     */
    void set_pin_payload_ratio(vertex_handle_t v, std::uint32_t k);
    /**
     * @brief Block until the vertex's value changes or @p timeout elapses; return the value.
     * @return The stored value as a rope, or a `status_t` (e.g. `TIMEOUT`).
     */
    [[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,
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
     * @brief COMPOSED BRANCH READ (RFC-0005 §C follow-on): the POINT tree of @p v's
     *        registered subtree, folded as a scatter-gather **rope** of views over the
     *        live last-known-value ropes (zero flatten, zero byte copies).
     *
     * `composed(target) = POINT{ [stored TLV of target]?, child_node* }` and
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
     * `wire::emit_tlv`). The walk is an ITERATIVE stack machine over a HEAP-BACKED stack, so
     * it needs no synthetic cap: the bound is the allocator, and exhaustion is `BACKPRESSURE`.
     *
     * It does NOT rely on `kMaxSegments`, and this comment used to claim it did ("graph depth
     * is `kMaxSegments`-bounded structurally"). That claim is false: `kMaxSegments` is enforced
     * only in `path_t::parse` (`core/src/path.cpp:110`), the LOCAL string→bytes builder.
     * `ensure_vertex` takes raw key bytes and counts nothing, so a wire-driven write-create
     * already registers a vertex at any depth. The iterative walk is safe because it is
     * iterative and resource-bounded — which is the real reason, and the only one that survives
     * `kMaxSegments` being lifted.
     *
     * Resolver contract: with a subject resolver installed, `acl_allows` — and therefore
     * the resolver callback — runs O(nodes) times per composed read **under the shared
     * `map_mutex_`**; a resolver MUST NOT re-enter graph mutation APIs (self-deadlock).
     */
    [[nodiscard]] result_t<rope_t> read_subtree_folded(vertex_handle_t v,
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
     *
     * @param policy This subscription's DELIVERY policy (RFC-0022 §3.A) — the same packed
     *               16 bits a wire subscriber sends in its `SETTINGS` child, and encoded
     *               into exactly that child here so the two doors stay byte-identical.
     *               Defaulted to all-zero: best-effort, default priority, no durability
     *               request — today's behaviour for every caller that says nothing.
     */
    [[nodiscard]] result_t<void> subscribe(const path_t& src, const path_t& target,
                                           delivery_policy_t policy = {});
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
     * @param policy This subscription's DELIVERY policy (RFC-0022 §3.A); defaulted to
     *               all-zero, i.e. today's behaviour. A callback edge carries no TLV, so
     *               the policy is set on the slot directly rather than parsed out of one.
     * @return A @ref subscription_t handle for @ref unsubscribe; error on an unknown @p src
     *         or a denied SUBSCRIBE gate.
     */
    [[nodiscard]] result_t<subscription_t> subscribe(const path_t& src, subscriber_fn_t fn,
                                                     void* ctx, delivery_policy_t policy = {});

    /**
     * @brief Subscribe @p src to a caller-owned callable (sugar over the `{fn, ctx}` form).
     *
     * Zero-erasure sugar mirroring `transport_t::set_receiver`: @p callback is bound by
     * address (lvalues only — a temporary would dangle) and MUST outlive every delivery.
     * @param policy This subscription's DELIVERY policy (RFC-0022 §3.A); all-zero default.
     * @return A @ref subscription_t handle for @ref unsubscribe (as the `{fn, ctx}` form).
     */
    template <typename F>
        requires std::invocable<F&, const view::rope_t&>
    [[nodiscard]] result_t<subscription_t> subscribe(const path_t& src, F& callback,
                                                     delivery_policy_t policy = {}) {
        return subscribe(
            src, [](void* c, const view::rope_t& v) { (*static_cast<F*>(c))(v); }, &callback,
            policy);
    }

    /**
     * @brief Remove the in-process subscription @p sub returned by @ref subscribe.
     *
     * The host-SDK-sugar counterpart of the wire `:subscribers[N]` clear (ADR-0049): it
     * deactivates the edge slot and unwinds the RFC-0005 listener bookkeeping (descendants'
     * writes stop bubbling to the producer @p sub names), exactly as the wire path does. The shell
     * stays (index-stable) and a later @ref subscribe reuses it. Idempotent-ish: a
     * default-constructed or already-cleared handle returns `NOT_FOUND`. Only DEACTIVATES —
     * an in-flight delivery already snapshotted the edge and completes (ADR-0041 §2), so the
     * callback's `ctx` must outlive any delivery that may still be running.
     * @note Applies to the callback-form subscriptions; a path→path (`subscribe(src, target)`)
     *       edge is a wire `:subscribers[]` field-write, removed via that wire clear.
     */
    [[nodiscard]] result_t<void> unsubscribe(const subscription_t& sub);

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
     * Idempotent and re-callable; the last install wins. CONFIGURATION, like
     * @ref register_child_type — install before frames flow.
     *
     * Install, `clear_identity` and `read_identity` nevertheless serialize on one
     * lock (#1049), because this is the one member on that list whose READ is served ABOVE
     * the READ gate — an unauthenticated peer may pin the key on first use (RFC-0011 §C),
     * which is deliberate. The read memcpys the stored record, so a rotation racing it would
     * otherwise be a remotely-reachable use-after-free. All three verbs are cold, so the
     * lock is invisible; a runtime rotation is therefore SAFE here, merely outside the
     * doctrine.
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
     * declaration RAM**: the runtime views @p table itself, so the caller MUST keep
     * **@p table and the bytes it points at** alive for the vertex's lifetime (pass a
     * `static`/`constexpr` array in flash / `.rodata`, never a stack array or a soon-freed
     * heap block). Note this is the ARRAY as well as its bytes — an earlier revision copied
     * @p table's entries into an owned vector, so only the bytes had to outlive the vertex,
     * and the "zero declaration RAM" above was untrue by ~200 B per vertex on host
     * (ADR-0058 erratum 1; measured by the `vertex_app5_static` gate row). Declaration
     * only — no initial value; write values later through the field-write surface. Empty
     * @p table uninstalls, exactly as @ref set_app_fields. Wire-invariant: `:schema` serves
     * the same verbatim bytes as the owning overload.
     *
     * @p table is a @ref borrowed_fields_t, which converts implicitly from the array
     * spellings a static table takes and NOT from a `std::vector` — so the erratum-1
     * lifetime tightening lands on a stale caller as a compile error rather than silently
     * (ADR-0058 erratum 2). A runtime-sized table opts out via `borrowed_fields_t::unchecked`.
     */
    void set_app_fields_static(vertex_handle_t v, borrowed_fields_t table);

    /**
     * @brief Install the sink the producer fan-out hands each REMOTE subscriber's delivery
     *        to (#136, RFC-0004 §D/§E.1).
     *
     * CONFIGURATION, not a runtime knob (#1049): install it at wiring time, from ONE thread,
     * before frames flow. The verb is named `configure_` to say so in the API rather than in
     * a comment asking callers to be careful — `tr::net::fwd_router_t`'s constructor installs
     * it, and a router constructed against a graph that is already serving frames is
     * UNSUPPORTED. The sink then fires on whatever thread calls @ref write (outside the
     * vertex lock), and on @ref subscribe for a transient-local latch. L4 keeps it as an
     * opaque function pointer, so the graph never depends on a transport. A null @p fn (the
     * default) ⇒ remote slots are stored but never deliver. The value reaches the sink as a
     * rope (ADR-0053 §6): a single-link value materializes zero-copy, a multi-link value is
     * handed over as the rope it is.
     *
     * The pair is published through a @ref tr::sink_slot_t, so violating the contract is
     * DEFINED rather than undefined: a fan-out racing an install either sees the whole new
     * pair, the whole old one, or no sink for that one edge — never a new `fn` beside a
     * stale `ctx`, and never the freed capture state the `std::function` predecessor could
     * hand it. What the slot does NOT do is stop a dispatch already in flight, so @p ctx
     * must outlive every write that can still reach the fan-out.
     *
     * @param fn  The sink; @p ctx is handed back as its first argument. Null clears.
     * @param ctx Caller-owned context; must outlive every possible dispatch.
     */
    void configure_remote_delivery_sink(remote_delivery_fn_t fn, void* ctx) noexcept;

    /**
     * @brief Install the pluggable subject resolver (ADR-0018) — the ACL enforcement switch.
     *
     * No resolver (the default) ⇒ enforcement is DISABLED: every operation is allowed,
     * exactly today's behavior, and the hot path pays one null check. With a resolver
     * installed, each gated operation with a NON-EMPTY caller context maps it through the
     * resolver and — when a subject token comes back — evaluates the target vertex's
     * *effective* ACL (own ACEs + ancestor ACEs carrying INHERIT, ADR-0020): allowed iff
     * some non-expired ACE with a matching subject (or `"EVERYONE@"`) grants the
     * operation's right bit; a vertex whose effective ACL is empty stays open (enforcement
     * is opt-in per vertex via ACL presence). Denial returns status_t::PERMISSION_DENIED
     * (`tr::access::denied` on the wire, RFC-0002).
     *
     * The EMPTY caller context is the local-API convention and is trusted WITHOUT consulting
     * the resolver (#905) — a remote op always carries its inbound link NAME, so it cannot
     * spell the trusted context. The resolver's own error arm is therefore free to mean
     * DENY: an unresolvable caller is refused, not waved through.
     *
     * The wildcard spelling is RESERVED against the resolver's OUTPUT (#908): a token equal to
     * `tr::graph::kEveryoneSubject` is not a principal — that caller is refused at every gate,
     * guarded vertex or not — because the wire has one spelling for a subject token, so a
     * resolver that passes a caller-supplied identity through could otherwise mint a principal
     * indistinguishable from the wildcard ACE.
     *
     * CONFIGURATION, not a runtime knob (#1049): install it at wiring time, from ONE thread,
     * before frames flow — which the verb's name now says, and the `{fn, ctx}` shape makes
     * safe to get wrong. The gate reads the pair through a `tr::sink_slot_t`: with no
     * resolver that is ONE relaxed load, exactly what the null check cost, and with one
     * installed the gate dispatches from a coherent snapshot, so a concurrent
     * install/replace can neither pair a new `fn` with a stale `ctx` nor free the state a
     * running resolver is standing on. @p ctx must outlive every gated operation.
     *
     * @param fn  The resolver; @p ctx is handed back as its first argument. Null (the
     *            default) DISABLES enforcement.
     * @param ctx Caller-owned context; must outlive every gated operation.
     */
    void configure_subject_resolver(subject_resolver_fn_t fn, void* ctx) noexcept;

    /**
     * @brief Install the EXTERNAL subscription observer — a callback fired on every
     *        `:subscribers[]` mutation that arrived over a transport.
     *
     * The app-side answer to "who is watching what, right now": a producer that wants to
     * spin up a source only while a peer is subscribed, an inventory of live remote
     * subscriptions, a projection of the fan-out graph. Today that is discoverable only by
     * polling `read_subscribers` over every vertex; this is the edge-triggered form.
     *
     * Fires from the ONE admission door every subscribe lands in (ADR-0049
     * `admit_subscriber`) and from the `:subscribers[N]` clear, so an append, a `[N]`
     * replace (a `REMOVED` for the displaced edge then an `ADDED`) and a clear are all
     * reported, whichever wire shape carried them — the wire `:subscribers[]` APPEND that
     * binds a REMOTE subscriber (`subscribe_wire`, target empty) and the one that names a
     * LOCAL target alike.
     *
     * **Only EXTERNAL mutations fire it** — see @ref sub_event_t for what that means and
     * why. Two further silences are by design, not oversight:
     * - `evict_link_edges` — the transport-plane hook that drops a departed link's edges
     *   wholesale (RFC-0009 §D) — emits NOTHING. It is a local host API, not an op, and it
     *   clears k edges of one link in a batch. An app tracking live subscriptions must
     *   therefore treat its own link-down signal as the removal for every edge of that link.
     * - `unsubscribe(subscription_t)` is a local door and stays silent like the rest.
     *
     * CONFIGURATION, not a runtime knob (#1049): install it at wiring time, from ONE thread,
     * before frames flow — the `configure_remote_delivery_sink` /
     * `configure_subject_resolver` contract, now stated by the verb and enforced by the
     * `{fn, ctx}` shape rather than requested in a comment. A null @p fn (the default) costs
     * one relaxed load on the subscribe path and nothing anywhere else. @p ctx must outlive
     * every subscription mutation the graph can still report.
     *
     * @param fn  The observer; @p ctx is handed back as its first argument. Null clears.
     * @param ctx Caller-owned context; must outlive every reportable mutation.
     */
    void configure_subscription_observer(sub_observer_fn_t fn, void* ctx) noexcept;

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
     * `PERMISSION_DENIED` on denial) → slot append → durability latch (if the parsed
     * `delivery_policy_t` sets `durability_request` and @p v holds a value, the LKV is
     * latched to this subscriber — one synchronous sink call, RFC-0004 §D / RFC-0022 §3.A).
     *
     * @p return_route MUST be non-empty — an empty one is `INVALID_PATH` (#1055). This door
     * is the only one that binds a link for delivery, so it is where the two fields are held
     * to ONE meaning: an edge that carries a link carries the route to deliver over it. The
     * fan-out body (`dispatch_edge`) therefore tests the link alone and hands the sink the
     * route unchecked, which is what keeps that deliberately-inlinable per-edge test at one
     * comparison; admitting a routeless edge instead bought a `FWD{WRITE}` with a zero-byte
     * `dst` on every publish. Both in-tree callers already satisfy this (the resolver rejects
     * a failed route copy as `BACKPRESSURE`, `fwd_router_t::subscribe_toward` refuses an empty
     * residual as `INVALID_PATH`), so the door narrowed to what the wire already produced.
     *
     * @p reverse_route, when non-empty, is the COMPLETED reverse-direction bound route
     * (RFC-0024 §7.1 amendment 1): a `PATH_REF` TLV whose element 0 is THIS node's own
     * reference to the connection vertex the subscribe arrived on, followed by the elements
     * the forwarding hops contributed. Stored beside @p return_route as the delivery
     * optimisation + liveness check; empty (the default, and every pre-amendment caller)
     * keeps the subscription canonical-only, byte-identical to before.
     */
    [[nodiscard]] result_t<void> subscribe_wire(vertex_handle_t v, view_t source_view,
                                                view_t return_route, std::string link,
                                                view_t reverse_route = {});

    /**
     * @brief Read by path — resolve the path key once (guarded map lookup), then the hot path.
     *
     * A read whose path has a field tail (e.g. `:settings.app.kp`, `:subscribers[]`,
     * `:schema`) is routed to the field surface.
     */
    [[nodiscard]] result_t<value_ref_t> read(const path_t& path) const;
    /** @brief Write by path — resolve the key once, then @ref write(vertex_handle_t, rope_t,
     * std::string_view). */
    [[nodiscard]] result_t<void> write(const path_t& path, rope_t value);
    /** @brief Await by path — resolve the key once, then @ref await(vertex_handle_t,
     * std::chrono::nanoseconds, std::string_view). */
    [[nodiscard]] result_t<value_ref_t> await(const path_t& path, std::chrono::nanoseconds timeout);

    /** @brief Resolve a canonical PATH-payload @p key to its vertex handle (`nullopt` if
     *         unknown). */
    [[nodiscard]] std::optional<vertex_handle_t> find(std::span<const std::byte> key) const;

    /**
     * @brief @p v's declared RFC-0022 §3.D pin amplification ratio `K` (ADR-0042 §3);
     *        @ref tr::graph::kPinNever (0) ⇒ this vertex never pins.
     *
     * The read accessor the opaque handle does not expose directly: the WRITE resolver
     * (`%op_resolve_walk.hpp`) queries it here instead of dereferencing the vertex. One
     * inline load, and nothing is inherited (RFC-0022 §3.F) — a vertex whose owner never
     * called @ref set_pin_payload_ratio answers 0 whatever its ancestors hold.
     */
    [[nodiscard]] std::uint32_t pin_payload_ratio(vertex_handle_t v) const noexcept;

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

    /**
     * @brief How many target-edge deliveries fell back to the canonical `find_ptr` walk
     *        instead of the minted binding (#830) — instrumentation.
     *
     * The inverse of a hit counter ON PURPOSE: this is the only path #830 leaves paying the
     * O(depth) resolve, so counting it costs the fast path nothing at all — no atomic on the
     * bound leg. Non-zero means one of: the edge was admitted before its target existed (or
     * against a placeholder / saturated generation, so no mint was possible), or the binding
     * went stale and `deref_vertex_slot` refused it. Relaxed monotonic counter, and the
     * observable an ablation uses to prove the bound leg is the one actually running.
     */
    [[nodiscard]] std::uint64_t target_canonical_resolves() const noexcept;

    /**
     * @brief Why a delivery was declined, counted per cause.
     *
     * A path-target edge — the form a wire `SUBSCRIBER` produces, naming a target PATH —
     * delivers by re-dispatching into that target. Three conditions make that impossible,
     * and all three are specified to DROP the one delivery rather than fail the write: the
     * write itself succeeded and the other legs still ran. Dropping is correct. Dropping
     * *invisibly* is what these counters fix — a node whose target was retired, or whose
     * fan-in gate denies the edge's stored caller, otherwise drops every delivery for the
     * rest of its life with nothing anywhere to say so.
     *
     * Two counters reach past that edge, because the same blindness was reachable from the
     * net plane (#1068). A COMPACT terminus delivery is a write like any other, and the
     * router that performs it discards the status: an `:acl` that refuses the inbound link,
     * a route that no longer resolves, or an allocation that fails under pressure each shed
     * a frame that an operator could not see. @ref count_external_drop is the door that
     * plane counts through, and @ref denied is counted at the graph's own WRITE gate so it
     * is one number for every plane rather than one per deliverer.
     *
     * The drop is not always ONE delivery, and the counters say so by counting deliveries
     * rather than events (#896): a fan-out truncated by an unreservable overflow buffer
     * sheds every edge past the inline prefix, and a handler write whose notify clone
     * cannot be allocated sheds the vertex's WHOLE subscriber set — each shed delivery is
     * one increment, so `1` never stands in for `N`.
     *
     * Counted, never enforced: nothing in the library reads them, so a deployment chooses
     * whether to alarm. Relaxed monotonic, incremented only ON a drop — the delivering path
     * pays nothing when nothing is dropped, exactly like @ref ancestor_walks.
     */
    struct delivery_drops_t {
        /** @brief The target PATH resolved to no live vertex (retired, or never created) —
         *         a subscription edge's target, or a net-plane route that no longer names
         *         one (@ref count_external_drop). */
        std::uint64_t no_target = 0;
        /** @brief A WRITE was refused by the target's `:acl` (#81, #1068). Counted on EVERY
         *         plane the value-write path is entered from — an API `write`, a
         *         FWD{WRITE} terminus, a COMPACT terminus, and a subscription edge's
         *         fan-in gate — so this is "refusals", not "refusals nobody was told
         *         about": an API caller both receives `PERMISSION_DENIED` and counts here.
         *         Deliberately NOT counted: `assign` (the no-delivery state half), a
         *         control-plane field write, and a denied READ — each a different right or
         *         a different path, and folding them in would make one number mean four
         *         things. */
        std::uint64_t denied = 0;
        /** @brief The nothrow delivery clone / edge-view copy could not be allocated
         *         (#477) — one count per delivery shed, whatever the fan-out width. */
        std::uint64_t out_of_memory = 0;
        /** @brief Deliveries shed because a wide fan-out's snapshot could not be widened
         *         past the inline prefix — a capacity degrade, not an allocation failure
         *         on the delivery itself (`vertex_t::snapshot_drops_t::truncated`). */
        std::uint64_t fan_out_truncated = 0;
    };

    /**
     * @brief Snapshot the per-cause delivery-drop counters (@ref delivery_drops_t).
     *
     * The loads are individually relaxed and not one atomic snapshot, so a reader
     * racing a delivering thread may see a torn total. That is deliberate: making it
     * coherent would put a lock on the drop path to serve a diagnostic, and these are
     * monotonic counters whose useful reading is "is this growing", not an instant.
     */
    [[nodiscard]] delivery_drops_t delivery_drops() const noexcept;

    /**
     * @brief Why a deliverer OUTSIDE the graph abandoned a delivery before it could write.
     *
     * Narrow on purpose (#1068). It names only the two ways a net-plane delivery dies
     * without ever reaching @ref write — the route resolves to no vertex, or the payload
     * view cannot be allocated. There is deliberately no `DENIED`: a refusal happens AT the
     * graph's own WRITE gate, which counts it there, so offering it here would let one
     * refusal be counted twice by a caller that also saw `PERMISSION_DENIED`.
     */
    enum class external_drop_t : std::uint8_t { NO_TARGET, OUT_OF_MEMORY };

    /**
     * @brief Count `n` deliveries an off-graph deliverer declined, into @ref delivery_drops.
     *
     * The ONE public door to the drop counters (#1068). The net plane performs deliveries
     * the graph never sees — a COMPACT terminus resolves a label to a vertex and writes it
     * — so the drops on that path are invisible to every counting site inside `graph_t`.
     * This is a method rather than a friendship because the counters are a public,
     * documented surface while the internal drop sites are not: a deliverer needs to add to
     * the published numbers, not to reach into the machinery that maintains them.
     *
     * @p n is a delivery count, never an event count, exactly as for the internal sites: a
     * deliverer that sheds N deliveries counts N. Relaxed monotonic; costs nothing when
     * nothing is dropped.
     */
    void count_external_drop(external_drop_t why, std::uint64_t n) noexcept;

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
    [[nodiscard]] result_t<void> write_impl(vertex_t* v, rope_t value, std::string_view caller);
    // The store half of a write (LKV/history/handler + seq bump + await wake),
    // WITHOUT fan-out — shared by write_impl and the branch-write apply (RFC-0005).
    // Hands back the exact published LKV pointer (null for a Handler-role write —
    // the user handler consumed the value, nothing was stored), so the eager write
    // path delivers precisely what was stored (RFC-0008 §D) without a rope reclone.
    // Takes `rope_t&&`, NOT by value (#1116). By value, a caller holding an lvalue built
    // a move-constructed temporary at the call site and destroyed it again — on the
    // per-delivery path-target leg that is once per subscriber per publish. Whether
    // that move was a few SSE stores or an out-of-line call depended on the inliner's
    // budget for this TU, which is what made an unrelated header change measurable as
    // a latency regression (#888/#1086). An rvalue reference binds what the caller
    // already owns, so there is no temporary to build and none to destroy.
    // NOTE the asymmetry this creates: the Handler leg never moves from `value`, so the
    // CALLER's rope now survives the call on that path holding its refcounts, where the
    // by-value temporary used to die at the call. Destruction count is unchanged.
    // `drops` reports what the store SHED (vertex_t::store_drops_t), zeroed on entry. REQUIRED,
    // not defaulted, and that is the whole point (#1003): this is the ONE funnel every graph
    // write reaches vertex_t::store through, so a required out-param is what makes "a write
    // path that abandons a delivery without counting it" impossible to write by omission —
    // the same reason snapshot_edges takes its tally by reference. Whether a shed cost a
    // DELIVERY is the caller's call; count_store_drops is where each site records its answer.
    // `caller` is the ACL caller context this store runs under — the very value the WRITE
    // gate one frame up just evaluated — and it becomes `write_ctx_t::subject` on the
    // HANDLER leg (#375). REQUIRED, not defaulted, for the same reason
    // `fwd_router_t::deliver_local`'s is: empty means the trusted local host, so a defaulted
    // parameter would let a new write path silently present a remote write to a handler as
    // the owner's own.
    [[nodiscard]] result_t<std::shared_ptr<const rope_t>> store_value(
        vertex_t* v, rope_t&& value, vertex_t::store_drops_t& drops, std::string_view caller);
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
    [[nodiscard]] result_t<void> write_branch(vertex_t* v, const rope_t& value,
                                              std::string_view caller, bool notify);
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
    // The cause a delivery was declined for — the argument of the ONE counting door
    // below. Kept private: the enum names the internal drop sites, while the public
    // surface is delivery_drops_t, whose fields are what an operator reads.
    enum class drop_reason_t : std::uint8_t { NO_TARGET, DENIED, OUT_OF_MEMORY, FAN_OUT_TRUNCATED };
    // Count `n` declined deliveries against `why` — the SINGLE door every drop site goes
    // through (#896). It exists because the three sites that forgot to count were the
    // three that incremented nothing rather than the wrong thing: a path that abandons an
    // admitted delivery and does not call this is now the visible omission it should be.
    // `n` is a delivery count, never an event count — a shed fan-out of N counts N.
    void count_drop(drop_reason_t why, std::uint64_t n) noexcept;
    // Fold one snapshot's shed tally (vertex_t::snapshot_drops_t, reported by
    // snapshot_edges) into the per-cause counters. Called on the fan-out path, so it
    // early-outs on the clean case in one test.
    void count_snapshot_drops(const vertex_t::snapshot_drops_t& drops) noexcept;
    // Fold one store's shed tally (vertex_t::store_drops_t, reported by store_value) into the
    // per-cause counters, at the width a shed STREAM ring append actually sheds: ONE PER
    // SUBSCRIBER of `v`, never one per event, matching the eager handler-clone leg. Call ONLY
    // from a site where the ring drain is the delivery — a branch NOTIFY fans its slice out
    // eagerly and flushes the cursor, so its shed costs history, not a delivery, and counting
    // it there would be an overcount. Early-outs on the clean case in one test.
    void count_store_drops(vertex_t* v, const vertex_t::store_drops_t& drops) noexcept;
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
    // `delivered` is the LKV pointer this caller's own store published (the handler leg's
    // null "consumed" sentinel included): the erase happens under the sweep lock only while
    // that is still v's CURRENT LKV, so a mark left by an assign that raced this write —
    // whose newer value this writer never delivered — survives instead of being dropped
    // (#1185, the #854-survivor locked-erase drop).
    void clear_pending(vertex_t* v, const std::shared_ptr<const rope_t>& delivered);
    // Subscribe/unsubscribe bookkeeping (RFC-0005): bump v's own active-slot count
    // and every descendant's listeners_above_, under the map lock (shared — the
    // counters are atomics; the lock only excludes concurrent vertex creation so
    // a newborn's creation-time sum and this walk never double-count).
    void note_subscriber_added(vertex_t* v);
    void note_subscriber_removed(vertex_t* v);
    // Record that `v` may hold an edge admitted over `link` (#1071 — see link_index_).
    // Called from the ONE admission door, at the note_subscriber_added bump, with no other
    // lock held. An empty `link` is a no-op: that is the LOCAL spelling, and a local edge is
    // not reachable by any link teardown (the #1056 empty-key rule, mirrored here so the
    // index cannot grow an entry no eviction can ever name). Idempotent AT THE INSERT (#1266,
    // which is where this promise finally became true) — a vertex already listed for `link`
    // is not listed twice, which is what keeps a peer that re-subscribes to the same vertex
    // from growing its own departure cost, its arena footprint, or its sort bill.
    void index_link_vertex(std::string_view link, vertex_t* v);
    // The candidate vertices an eviction for `link_name` must visit — see the definition.
    // `take` removes the index entry (whole-link teardown) instead of copying it
    // (route-scoped reclaim, which leaves the link holding other edges).
    [[nodiscard]] std::pmr::vector<vertex_t*> link_candidates(std::string_view link_name,
                                                              bool take);
    // The single SUBSCRIBER admission step (ADR-0049): SUBSCRIBE gate under `caller` →
    // slot append → transient-local durability latch (delivered outside the lock, per
    // the edge's kind) → RFC-0005 bookkeeping. Every door — the two subscribe() sugars,
    // the local `:subscribers[]` field-write, and the wire subscribe_wire — ends here,
    // so gate and latch semantics cannot diverge per entry point.
    [[nodiscard]] result_t<subscription_t> admit_subscriber(
        vertex_t* v, subscriber_t s, std::string_view caller,
        std::optional<std::size_t> slot = std::nullopt);
    // Fire the external-subscription observer for ONE slot mutation
    // (configure_subscription_observer). Returns immediately when no observer is installed or
    // `caller` is EMPTY — the latter is the whole external/local discrimination, in one place.
    // `sub_tlv` is the slot's stored SUBSCRIBER TLV (empty for a callback-only slot, which no
    // external door can create); the event's target key is decoded from its PATH child. Called with
    // NO graph lock held, after the mutation has landed — see sub_observer_fn_t's re-entrancy
    // warning.
    void notify_subscription(sub_event_t::kind_t kind, const vertex_t* v, std::string_view caller,
                             const view_t& sub_tlv, std::size_t slot) const;
    // True iff a subscription event is worth building at all — an installed observer AND an
    // external (non-empty) caller context. Guards the pre-reads the observer needs (the
    // displaced slot's stored SUBSCRIBER on a replace/clear) so an app that installs nothing
    // pays exactly one relaxed load. A HINT only: `notify_subscription` re-reads the slot
    // coherently and dispatches from THAT snapshot, so a clear landing between the two
    // simply drops the event rather than calling a destroyed target (#1049).
    [[nodiscard]] bool observing_subscriptions(std::string_view caller) const noexcept {
        return subscription_observer_.installed() && !caller.empty();
    }
    // Field surface: ":settings.<f>", ":settings.app.<name…>" (RFC-0010),
    // ":subscribers[]" / "[N]", ":children[]".
    [[nodiscard]] result_t<void> field_write(vertex_t* v, const field_path_t& field,
                                             const view_t& value, std::string_view caller);
    // The ACL gate (#81, ADR-0018/0020): true iff `caller` may exercise `right` on
    // `v`. True with no resolver installed (one null check — enforcement off), for the
    // trusted EMPTY (local) caller — settled before the resolver runs, #905 — or when
    // the effective ACL (own ACEs + INHERIT-flagged ancestor ACEs) is empty. FALSE
    // outright when the resolver refuses to name the caller; otherwise the verdict of the pure
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
    [[nodiscard]] result_t<void> create_child(vertex_t* parent, const view_t& spec_value);
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
    // ":acl" read => the stored ACEs RE-ENCODED (#81-A, ADR-0018/0020, #907) — a projection
    // of the list acl_allows walks. The READ_ACL gate runs in read(v, field, caller).
    [[nodiscard]] result_t<view_t> read_acl(vertex_t* v) const;
    // Bare ":settings" read (RFC-0010 §A.4 as amended by RFC-0022 §4) => the settings
    // container: the reserved `app` record iff a descriptor table is installed, and
    // NOTHING else — the core knob namespace is empty. An empty SETTINGS{} is the honest
    // answer for a vertex that declares no app fields.
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
    // The NOTHROW twin of build_key for the writer-thread store/delivery legs (#477):
    // renders into `out` via the mem_heap.hpp nothrow growth primitives; false on OOM
    // (out is cleared), so a sweep-set mark/drain leg drops or defers instead of a
    // bad_alloc abort() under the MCU profile's -fno-exceptions.
    [[nodiscard]] static bool try_build_key(const vertex_t* v,
                                            std::vector<std::byte>& out) noexcept;
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
    // pointer — it is PARKED here (ADR-0057 insert-only, applied to the seam). Kept on the
    // GRAPH, not per-vertex, so an app-field / leaf vertex pays zero extra bytes. Appended
    // only under map_mutex_ (unique). #576: the park's other end is the public collect(),
    // which the EMBEDDER calls at a moment it knows no reader holds a seam. The graph's own
    // destructor is a growth backstop only, not a substitute: this member is declared BEFORE
    // map_mutex_ and root_, so it destructs LAST — a seam whose destructor re-enters the
    // graph finds a half-destroyed object. Until collect() runs the size is peer-driven (one
    // per BUS-link connection teardown; a point-to-point teardown parks nothing, because the
    // identity vertex only gets an on_children when link->bus() != nullptr) — hence the
    // public parked_seam_count().
    std::vector<std::unique_ptr<value_handlers_t>> retired_seams_;

    // The node-scoped vertex index (RFC-0024 §6.4) — the ONE new structure a bound path
    // needs, named honestly. The "vertex map" is a Composite tree of non-moving unique_ptr
    // allocations with no dense index, so an element's u32 index has no meaning until one
    // exists. This is it: one slot appended per vertex_t ALLOCATION (slot 0 = root_,
    // placeholders included), under the same unique map_mutex_ hold that links the node into
    // the tree, so slot order is allocation order and the mapping is a bijection forever.
    //
    // It is NOT a route table: its size tracks the graph, not the traffic, so it does not
    // reintroduce the per-flow state a bound path exists to avoid. 4 B/vertex on rv32, 8 on a
    // host — the figure RFC-0024 §4.4's RAM floor already charges. Appending per allocation
    // rather than per REGISTRATION is what keeps it a bijection: a retired vertex is revived
    // by a second fill() of the same object, which must not mint a second slot.
    //
    // Session identity anchors (@ref register_session_anchor, #1223) append here on the same
    // terms and for the same reason — an anchor is allocated once and revived in place, so a
    // slot handed out for one names that allocation forever. The one structural exception is
    // `anchor_root_` itself, which takes NO slot: it is a private parent, never registered,
    // never filled and never mintable, so the property this container actually owes RFC-0024
    // — "every vertex a mint may be asked for has exactly one immovable slot" — is untouched.
    //
    // CHUNKED, not a `std::vector`, and the difference is the whole charged cost. A vector
    // grows geometrically, so between two doublings it holds up to TWICE the pointers it
    // needs: measured on the 512-vertex heap probe (bench_forward_heap `zeroheap vertex`) a
    // vector cost 15 B/vertex live where the RFC charges 8 — the slack, not the slot, was
    // most of it. A deque appends into fixed-size blocks, so live bytes track the vertex
    // count instead of the last doubling: the same probe reads 8 B/vertex, exactly the
    // pointer §6.4 prices and not a byte of unpriced headroom. Indexing stays O(1) and
    // elements never move, which is all the deref needs.
    std::deque<vertex_t*> vertex_slots_;

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

    /** @brief The ADR-0060 injected byte-buffer seam the write-path copy-store draws
     *         its owned value @ref view::segment_t from (the flatten of a branch/field
     *         write, `graph.cpp` sites 825/1017). Host-owned; outlives the graph.
     *         Defaults to the standard heap, so behaviour is byte-identical until a
     *         host injects a pool. MUST be thread-safe when injected (§2): a segment's
     *         reclaim self-routes on the last-ref thread, concurrent with a writer's
     *         alloc. On exhaustion it returns `nullptr` — the write BACKPRESSUREs
     *         (§3), never a silent heap fallback. */
    mem::mem_backend_t* value_backend_ = &mem::heap_backend();
    std::unique_ptr<vertex_t> root_;
    // The session identity anchors' private structural root (#1223, ADR-0044 §Amendment
    // 2026-08-13). NOT reachable from root_ — that is the whole design: an anchor gets a
    // vertex-map slot and a saturating generation without becoming an ADDRESS. `find` and
    // every `:children[]` listing walk from root_, so they never see one; a bus mount's
    // members stay exactly what `enumerate_peers` synthesizes, and RFC-0020 §3's "MUST NOT
    // resolve the residual against its local graph" keeps its premise, because there is no
    // local graph node below the mount for a residual to land on.
    //
    // Anchors ARE ordinary vertices otherwise, which is what makes them useful: same
    // pointer-stable insert-only allocation, same vertex_slots_ append, same fill()/retire
    // revive-in-place, same saturating retire_gen_. Giving them a parent (rather than
    // leaving them free-floating) is what lets `retire` — which refuses a parentless vertex
    // — work on them unchanged.
    //
    // Anchor keys cannot collide with an addressable vertex's key: build_key stops at the
    // node whose parent is null, so an anchor's key is its own single NAME record, and the
    // router composes that record's content to contain `:` and `/` — two of the SEVEN
    // characters `path::valid_segment` rejects, so no registered address anywhere in this
    // graph can render the same bytes. That is what keeps retirement's sweep-set cleanup
    // (which is keyed by rendered key bytes) from ever touching a real vertex's entry.
    std::unique_ptr<vertex_t> anchor_root_;
    // The device creation catalog (#82, ADR-0017): SPEC `type` -> factory. Populated at
    // setup (register_child_type) — configuration, per #1049's ruling, exactly like the
    // three sinks below. `std::less<>` enables heterogeneous string_view lookup.
    //
    // It is a std::map, so #1049's {fn, ctx} + slot mechanism does not reach it: there is no
    // pointer-sized word to publish, and the read walks a red-black tree an insert would be
    // rebalancing. The lookup runs from a PEER's bytes (create_child) and is control-plane
    // cold — one vertex creation — so the arm that costs nothing where it lands is a lock,
    // and it is taken here rather than on any hot path. It buys a defined outcome for a
    // caller that violates the setup-only contract instead of a corrupted tree walk.
    mutable std::shared_mutex child_types_mutex_;
    std::map<std::string, child_factory_t, std::less<>> child_types_;
    // The three CONFIGURATION sinks (#1049). Each is the ADR-0047 {fn, ctx} pair published
    // through a sink_slot_t — the mechanism #914 established for fwd_router_t's five, hoisted
    // to the layer-neutral `tr` namespace so L4 can hold one without naming the net plane.
    // The doctrine is setup-only and the verbs are named `configure_*` to say it; the slot is
    // what makes a violation DEFINED (a skipped dispatch) rather than the use-after-free the
    // std::function predecessors had, since assigning a std::function destroys the old
    // target — freeing its captures while a reader is inside the call. An unset slot reads as
    // one relaxed load, which is what the null check on the plain member cost.
    tr::sink_slot_t<remote_delivery_fn_t> remote_sink_;         // read on the write hot path
    tr::sink_slot_t<subject_resolver_fn_t> subject_resolver_;   // read by the ACL gate
    tr::sink_slot_t<sub_observer_fn_t> subscription_observer_;  // read on subscribe/clear
    // The NODE's identity record, pre-serialized (#406, RFC-0011 §B): the complete
    // SETTINGS{kind,key} TLV, built once at install so every `:identity` read is a copy
    // of settled bytes rather than a re-emit — the "all vertices return byte-identical
    // records" invariant (§C.1) then holds by construction, not by discipline. Empty =
    // no keypair installed => SCHEMA_NOT_FOUND (§C.3). Configuration, per #1049.
    //
    // Guarded, and this is the member where that is not merely tidy (#1049): the identity
    // facet resolves ABOVE the READ gate so an unauthenticated peer can pin the key on first
    // use, so `read_identity` is reachable, by design, from a peer that has proved nothing.
    // The read tests emptiness and then MEMCPYs the buffer; install and clear both free the
    // old one. Straddling that with a reassignment is a remotely-reachable use-after-free,
    // and the read is cold (one identity read per peer per pin), so the lock costs nothing
    // anywhere that matters. Same reasoning as child_types_ above: a std::vector has no
    // pointer-sized word for the slot mechanism to publish.
    //
    // The reader copies these bytes out through a stack buffer and allocates only after
    // unlocking, so this mutex is a strict LEAF — acquired at three sites, holding nothing
    // else, calling no allocator. That is what keeps it free of a lock-ordering obligation
    // once #873 injects a `block_source_t` (whose `Sync` policy may take its own lock) at
    // the allocation site.
    mutable std::shared_mutex identity_mutex_;
    std::vector<std::byte> identity_record_;
    // Bubbling-walk instrumentation (RFC-0005) — see ancestor_walks().
    mutable std::atomic<std::uint64_t> ancestor_walks_{0};
    // Canonical-fallback instrumentation (#830) — see target_canonical_resolves(). Touched
    // only when a target edge has no usable binding, so the bound leg pays nothing.
    mutable std::atomic<std::uint64_t> target_canonical_resolves_{0};
    // Per-cause delivery-drop instrumentation — see delivery_drops(). Touched only on the
    // drop path, so the delivering path is byte-identical while nothing drops.
    mutable std::atomic<std::uint64_t> drops_no_target_{0};
    mutable std::atomic<std::uint64_t> drops_denied_{0};
    mutable std::atomic<std::uint64_t> drops_oom_{0};
    mutable std::atomic<std::uint64_t> drops_truncated_{0};

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
    /** @brief The #551 nothrow failable-block seam. Host-owned; outlives the
     *         graph. Defaults to the platform heap, so behaviour is byte-identical
     *         until a host injects a bounded source — except that exhaustion is a
     *         `nullptr` return rather than the `-fno-exceptions` abort stub.
     *         Its first consumer is the branch-write decode's bump upstream
     *         (`graph.cpp`); registration and the remaining containers are still
     *         to migrate. Kept a DIFFERENT type from `mr_` on purpose (see
     *         @ref tr::mem::block_source_t).
     *
     *         LAST on purpose: no hot path reads it, so declaring it here keeps
     *         every other member at the byte offset it had before this seam
     *         existed. A cold pointer inserted mid-object would shift `root_` and
     *         everything after it, which is a layout change the forward-hop bench
     *         can see and nothing gains from.
     */
    mem::block_source_t* ctl_ = &mem::heap_source();

    // ---- #1071: the per-link departure index. LAST, beside `ctl_`, and for the same
    // reason that member documents: no hot path reads these, so declaring them here
    // keeps `root_`, `vertex_slots_` and every other member at the byte offset it had
    // before this index existed. Declared mid-object instead, they shifted those
    // offsets and `graph_t::fan_out` grew 48 B of wider displacement encodings — a
    // codegen change on the DELIVERY path, which the symbol ratchet caught and which
    // nothing here gains from.
    /**
     * @brief Transparent hashing/equality for `link_index_`, so a lookup spends no
     *        allocation turning a caller's `std::string_view` into a key (#1071).
     *
     * Hashed rather than ordered because this lookup sits on the SUBSCRIBE path: a tree's
     * cost grows with the number of live links, so an ordered index made every subscribe a
     * little dearer on a node with more peers — measurably, and in the one direction #1071's
     * acceptance criteria refuse. A hash makes it independent of that count.
     */
    struct link_key_hash_t {
        using is_transparent = void; /**< @brief Enables the heterogeneous `find`. */
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    /** @brief The equality half of `link_key_hash_t`'s heterogeneous lookup. */
    struct link_key_eq_t {
        using is_transparent = void; /**< @brief Enables the heterogeneous `find`. */
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    /**
     * @brief Which vertices hold a subscriber edge for a given LINK NAME — the index that
     *        makes a peer's departure cost its OWN edges instead of the graph's (#1071).
     *
     * Keyed by the edge's ADMITTED-OVER spelling, which is what `vertex_t::evict_link_edges`
     * matches on: `remote->link`, falling back to `remote->caller` when the former is empty
     * (a `field_write` admission stores the inbound link only as the gate context — #943).
     * Computing the key any other way here would silently un-index exactly the edges that
     * fix was about.
     *
     * A SUPERSET, deliberately, and that asymmetry is the whole safety argument. An entry is
     * added when an edge is admitted and removed only when the whole link is evicted, so an
     * individual unsubscribe, a replace that displaces the last edge of a link, and a failed
     * admission all leave a STALE vertex behind. A stale entry costs one
     * `vertex_t::evict_link_edges` that matches nothing and reports 0 — the same no-op the
     * old whole-tree walk performed on every unsubscribed vertex it visited. A MISSING entry
     * would instead leak a live edge past a departure, so every path that can create one
     * indexes, and no path except whole-link eviction removes.
     *
     * Insertion happens at the `note_subscriber_added` bump, NOT after the slot lands:
     * that bump is precisely the predicate (`own_subs() > 0`) the replaced whole-tree walk
     * keyed on, so the index becomes visible to a concurrent eviction no later than the walk
     * would have seen the vertex. The subscribe-racing-its-own-link's-teardown window is
     * therefore exactly the pre-existing one @ref evict_link_edges documents, neither
     * widened nor narrowed.
     *
     * Drawn from the injected `mr_` (ADR-0039): the departure path allocated a
     * global-heap `std::vector` sized to the graph's whole subscribed set on every peer
     * hangup, which is the allocation #1071 called out. It now allocates nothing at all —
     * the candidate list IS this entry, moved out.
     */
    /** @brief One link's candidate list, plus where its sorted prefix ends. */
    struct link_entry_t {
        std::pmr::vector<vertex_t*> vs; /**< @brief The link's DISTINCT candidate vertices:
                                         *          `[0, compacted)` sorted, then an unsorted
                                         *          tail (`graph_t::index_link_vertex`). */
        std::size_t compacted = 0;      /**< @brief Where the sorted prefix ends — `vs.size()`
                                         *          as of the last compaction. */
    };
    /** @brief How long a link's UNSORTED tail may get before it is merged into the sorted
     *         prefix — so the membership test's linear half stays a handful of pointers and
     *         the sort is paid per NEW vertex, never per subscribe. */
    static constexpr std::size_t kLinkIndexCompactFloor = 8;

    // `mutable` because the entries are a CACHE of where a link's edges may be: the
    // diagnostic reader compacts one in place to report a distinct count, which changes no
    // observable graph state. Guarded by the mutable mutex below, as `map_mutex_` is.
    mutable std::pmr::unordered_map<std::pmr::string, link_entry_t, link_key_hash_t, link_key_eq_t>
        link_index_{mr_};
    /** @brief Guards `link_index_` ONLY. A leaf: never held across a map, stripe, or
     *         sweep acquisition, so it orders against nothing else in this class. */
    mutable std::mutex link_index_mutex_;
};

}  // namespace tr::graph
