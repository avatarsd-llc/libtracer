/**
 * @file
 * @brief The L4 in-process graph runtime: the composite vertex tree and the read / write /
 *        await API.
 *
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
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
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
#include "libtracer/function_ref.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/link_id.hpp"
#include "libtracer/link_index.hpp"
#include "libtracer/mem_chunked_map.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_sorted_map.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_backend.hpp"
#include "libtracer/mem_string.hpp"
#include "libtracer/path.hpp"
#include "libtracer/reclaim.hpp"
#include "libtracer/sink_slot.hpp"
#include "libtracer/status.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/vertex_handle.hpp"
#include "libtracer/view.hpp"

namespace tr::wire {
class tlv_node_t;  // fwd-decl: the child factory takes a `const tlv_node_t*` config (no L2
                   // pull-in).
}

namespace tr::graph {

class graph_t;

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
 * (a `tr::net` concern — @ref graph_hooks_t::remote_delivery) interprets these:
 * it maps @ref link to a transport child and emits a full-route `FWD{WRITE}` or,
 * when @ref delivery_compact, an auto-promoted label `COMPACT` (RFC-0004 §D/§E.1).
 * @ref link is borrowed for the sink call only; @ref return_route is a refcount
 * clone of the stored route segment (ADR-0041 §2) — the sink may rope it into an
 * egress frame, and it stays alive across a concurrent unsubscribe.
 */
struct remote_delivery_t {
    std::string_view link;     /**< @brief This node's NAME for the consumer link. */
    view::view_t return_route; /**< @brief Consumer return route (PATH TLV view, refcount clone). */
    /** @brief Completed reverse bound route (`PATH_REF` view, refcount clone; empty ⇒
     *         canonical-only). Element 0 is this node's own reference, consumed locally by
     *         the sink per delivery — RFC-0024 §7.1 amendment 1. */
    view::view_t reverse_route;
    /** @brief The edge's stored ACL fan-in context (#81) — the subject the sink's local
     *         element-0 consumption re-checks §6.2 under. */
    std::string_view caller;
    bool delivery_compact = false; /**< @brief Opt-in to label-compacted delivery. */
};

/**
 * @brief The remote-delivery sink itself — what @ref graph_hooks_t::remote_delivery
 *        installs and the producer fan-out calls per remote subscription edge.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049) — see
 *       `subject_resolver_fn_t` for why. This is the one of the three on the WRITE hot
 *       path, so it is also the one where the `std::function` indirect call and its
 *       destroy-on-assign were most expensive; @p ctx is caller-owned (the
 *       `tr::net::fwd_router_t`) and must outlive every dispatch the graph can still make,
 *       exactly as `receiver_slot_t`'s context must.
 */
using remote_delivery_fn_t = void (*)(void* ctx, const remote_delivery_t& sub,
                                      const value_t& value);

/**
 * @brief Announce that THIS thread holds no `%edge_view_t` snapshot and drain whatever that
 *        made reclaimable — the quiescent point of a cross-thread grace period (ADR-0080).
 *
 * Compiles to nothing at all unless this build binds a policy whose grace point spans threads,
 * which today means @ref tr::graph::reclaim_qsbr_t. It does two things there, and both are
 * cheap-and-early-out rather than conditional on the caller knowing anything:
 *
 * 1. drains the shared retired table of every `{ctx, release}` pair whose grace period has now
 *    elapsed — guarded by one relaxed load of a count that is 0 on a node not mid-teardown;
 * 2. when this build also binds `%hazard_slot_t`, self-drains THIS thread's private retired LKV
 *    list via the already-shipped `%tr::graph::detail_hp::retire_and_flush`, which is the half
 *    of [#897](https://github.com/avatarsd-llc/libtracer/issues/897) ADR-0080 asks this policy
 *    to discharge: the displacing thread frees its own parked nodes at its own quiescent point,
 *    so `~hazard_slot_t` never has to reach across a live thread's private list. It costs
 *    `%lkv_slot.hpp` no code — the function's own early-out makes it free on a thread that
 *    parked nothing.
 *
 * Called automatically by every outermost dispatch exit; @ref graph_t::thread_quiescent is the
 * opt-in spelling for a thread that reaches a quiescent state WITHOUT dispatching.
 */
template <class P>
inline void pass_quiescent_state() noexcept {
    if constexpr (P::kGraceSpansThreads) {
        detail_qsbr::drain();
        if constexpr (std::is_same_v<lkv_slot_t, hazard_slot_t>)
            detail_hp::retire_and_flush(nullptr);
    }
}

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
 *
 * @section subscription_reclamation The reclamation guarantee this handle carries (ADR-0080)
 *
 * `unsubscribe()` retires the edge, but the fan-out path snapshots a vertex's edges and
 * dispatches OUTSIDE every lock, so a snapshot taken before the retirement still names the
 * subscriber's `{fn, callback_ctx}` pair — the one leg of an `%edge_view_t` snapshot the
 * library does not own a copy of. WHEN that pair becomes safe to free is therefore a real
 * question, and ADR-0080 answers it with a **build-time-closed, per-target policy**
 * (@ref tr::graph::default_config_t::reclaim_policy_t), not with a runtime contract asking
 * the caller to reason about in-flight state.
 *
 * **This build's guarantee is the one stated on the bound policy** —
 * @ref tr::graph::reclaim_local_t (the default), @ref tr::graph::reclaim_strict_t or
 * @ref tr::graph::reclaim_qsbr_t. Under all three the library owns the tracking and SIGNALS
 * release through the @ref tr::graph::subscriber_release_fn_t hook of
 * @ref graph_t::unsubscribe(const subscription_t&, subscriber_release_fn_t): the hook runs
 * exactly once, outside every graph lock, at that policy's grace point. **There is nothing to
 * poll and nothing to wait on** — that shape is precisely what ADR-0080 §Decision 4 rejects.
 *
 * The two per-thread policies state their guarantee over ONE thread's dispatch domain, which
 * is the single-threaded WIDE / MCU target they are for, and run the hook on the caller's own
 * thread. An embedder that dispatches from several threads at once and unsubscribes from
 * another needs a grace period spanning every thread: bind
 * @ref tr::graph::reclaim_qsbr_t. Its one API difference — the hook may then run on a thread
 * other than the `unsubscribe()` caller, because a cross-thread grace period cannot promise
 * otherwise without blocking — is stated on the policy itself.
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
 * @brief The subject resolver writing into CALLER storage (#1781): caller context → subject
 *        token, with no owning container crossing the seam.
 *
 * The same contract as @ref subject_resolver_fn_t — the error arm is a DENY, it is never
 * invoked with an empty caller, and a token spelling `EVERYONE@` is refused at every gate —
 * but the token is appended to @p out instead of returned. @p out is empty on entry and is
 * drawn from a stack-first frame the ACL gate owns, which spills to the graph's table source
 * only for a token past 64 bytes, so a gated operation allocates nothing for its subject. A
 * refused append (the source is exhausted) should answer `wire::err_t::ACCESS_DENIED`: an
 * unnamed caller fails closed.
 *
 * Install it as @ref graph_hooks_t::subject_lookup. It replaces @ref subject_resolver_fn_t,
 * which is removed once its callers have moved (ADR-0083 Decision 11).
 */
using subject_lookup_fn_t = std::expected<void, wire::err_t> (*)(void* ctx, std::string_view caller,
                                                                 mem::bytes_t& out);

/**
 * @brief One EXTERNAL mutation of a producer's `:subscribers[]` — what @ref
 *        graph_hooks_t::subscription_observer reports.
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
     * @warning Read it as the SPELLING the record carried, not as a local vertex. On a wire
     *          subscribe it is one of two things and the event cannot tell them apart: a path
     *          through one of THIS node's mounts, which `subscribe_wire` resolves and binds
     *          the edge to (RFC-0021 §4.B.1), or the consumer's address at ITS OWN root, which
     *          resolves to nothing here and is dropped as a re-dispatch target — delivery then
     *          rides the return route (RFC-0004 §D).
     *          On a local-target append it IS a key in this graph. The three are not
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
 *          APP's job, exactly as it is for @ref graph_hooks_t::remote_delivery.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049) — see
 *       `subject_resolver_fn_t` for why. @p ctx is caller-owned and must outlive every
 *       subscription mutation the graph can still report.
 */
using sub_observer_fn_t = void (*)(void* ctx, const sub_event_t& event);

/**
 * @brief What the transport plane made of a wire `SUBSCRIBER`'s `PATH` target, resolved
 *        from THIS (the producer's) root — RFC-0021 §4.B.
 *
 * The target is one ordinary address in the producer's own frame, so deciding whether it
 * leaves this node is a mount question and mounts are the transport plane's (ADR-0061's
 * strip-K descent over the child registry). L4 cannot name that plane, so the split arrives
 * through the `%wire_target_fn_t` seam and this struct is the whole of what L4 needs: the
 * link to deliver over, and the route to deliver with.
 */
struct wire_target_split_t {
    /**
     * @brief The matched mount's REGISTRY identity — what the delivery sink resolves the
     *        link by. EMPTY means no mount matched: the target names something local (or
     *        nothing), which this door does not bind (see `%wire_target_fn_t`).
     */
    std::string_view link{};
    /**
     * @brief The residual NAME records BELOW the mount — the delivery route the first hop
     *        forwards, exactly as an inbound `FWD`'s `dst` would carry it. Borrowed from the
     *        key handed to the seam, so it is valid only for the duration of the call.
     */
    std::span<const std::byte> residual{};
    /**
     * @brief @ref link's INTERNED identity, when the resolver had one to give (#1437).
     *
     * A mount-routed target REBINDS the key the subscriber index is written under, from the
     * link the subscribe arrived over to @ref link — so the token the arrival carried
     * (`subscribe_wire`'s `link_token`) names the wrong link and is correctly ignored, and
     * the admission falls through to the name door's scan. That is the one un-carried door
     * that runs per SUBSCRIBE rather than per hangup. A resolver that can answer this cheaply
     * — the transport plane can: a mount is a registered child with a durable word to keep
     * @ref graph_t::intern_link_hinted's slot hint in — closes it, and one that cannot leaves
     * this default and gets exactly today's behaviour.
     *
     * An OPTIMISATION and only that. It is checked against @ref link at the index door like
     * every other carried token, so a resolver that answers a token for some other link
     * degrades to the lookup instead of mis-indexing.
     */
    link_id_t token{};
    /**
     * @brief A mount WAS named, but it cannot carry a directed delivery — the target named
     *        the mount exactly (nothing below it), or its first hop landed on a bus link's
     *        own NAME (ADR-0073 §3 / RFC-0020). RFC-0021 §B.3: the subscribe-write is
     *        REJECTED, never silently degraded to the arrival-session binding (§F).
     */
    bool unroutable = false;
};

/**
 * @brief Resolve a wire `SUBSCRIBER`'s `PATH` target against this node's mounts (RFC-0021).
 *
 * @param ctx The caller-owned context installed beside the function.
 * @param key The target's canonical key — concatenated `NAME` records, the `PATH` payload.
 * @return The split; a default-constructed value (empty link, `unroutable == false`) means
 *         NO mount matched, on which @ref graph_t::subscribe_wire keeps today's
 *         arrival-session binding. The purely-local target of RFC-0021 §B.2 is deliberately
 *         NOT bound through this door yet — §7 open question 3 is unruled, and in-tree wire
 *         senders (the TypeScript client's `subscribe`, most `acl_test` subscribes) still
 *         spell the `PATH` in the CONSUMER's own frame, which §B.3 would reject outright.
 *
 * @note The ADR-0047 `{fn, ctx}` shape, NOT a `std::function` (#1049), published through a
 *       @ref tr::sink_slot_t like every other graph seam. Called at subscribe time only —
 *       never on the delivery path.
 */
using wire_target_fn_t = wire_target_split_t (*)(void* ctx, std::span<const std::byte> key);

/**
 * @brief One member of a `:stats` census block — a noun and its value (RFC-0010 Am. 2).
 *
 * The noun is BORROWED and must be a literal (or otherwise outlive the sampling call): the
 * block is encoded before the sampler's frame is left, and nothing copies the string.
 */
struct stats_counter_t {
    std::string_view noun{}; /**< @brief The `core/STYLE.md` §Introspection vocabulary name. */
    std::uint64_t value = 0; /**< @brief Its value, emitted as a fixed-width u64. */
};

/**
 * @brief One sampled seam block, filled by a `%stats_sampler_fn_t` (RFC-0010 Am. 2).
 *
 * A fixed-capacity, allocation-free carrier: the net plane samples INTO it and L4 encodes it,
 * so the whole census keeps ONE encoder and one wire shape (RFC-0010 Am. 1 §D.3), and the
 * sampling half never touches an allocator on a path a peer can drive.
 *
 * @ref kMaxMembers is a compile-time ceiling on how many nouns one seam may publish, not a
 * protocol limit: it is sized to the widest block the reference net plane serves
 * (`router_stats_t`'s seven) with headroom, and a seam that outgrew it would be split rather
 * than truncated — @ref add refuses silently past the ceiling exactly so a caller cannot
 * emit a half-member.
 */
struct stats_block_t {
    /** @brief The per-seam member ceiling — see the class brief. */
    static constexpr std::size_t kMaxMembers = 12;
    std::array<stats_counter_t, kMaxMembers> members{}; /**< @brief The filled prefix. */
    std::size_t count = 0;                              /**< @brief How much of it is filled. */

    /**
     * @brief Append one member; a no-op once @ref kMaxMembers is reached.
     * @param noun  Borrowed, and must outlive the sampling call.
     * @param value The counter's value.
     */
    void add(std::string_view noun, std::uint64_t value) noexcept {
        if (count >= kMaxMembers) return;
        members[count++] = stats_counter_t{noun, value};
    }
};

/**
 * @brief Sample one NET-PLANE `:stats` seam — the sixth `{fn, ctx}` seam the router installs
 *        UP into the graph (RFC-0010 Amendment 2, #1503 residual).
 *
 * Amendment 1 §D.4 drew the census boundary at the graph, because L4 cannot reach DOWN into
 * the net plane to sample a router or a link. This seam inverts the direction instead of the
 * dependency: the router, which already knows the graph, registers a sampler UP — the same
 * shape as `graph_hooks_t::remote_delivery` and the four resolver seams its constructor
 * installs — so L4 still names nothing below it.
 *
 * @param ctx        The caller-owned context installed beside the function.
 * @param seam_class The seam CLASS — `router`, `labels` or `link` (Amendment 2 §D.4).
 * @param seam_name  The seam NAME within that class; for `link` it is the router's
 *                   `child_registry_t` name.
 * @param out        Where to write the block, or `nullptr` for a RECOGNITION PROBE: answer
 *                   whether the spelling names a seam and sample nothing.
 * @return `true` when the spelling names a seam this sampler serves. `false` is the
 *         node's "this seam is not published here" — `SCHEMA_NOT_FOUND`, and per Amendment 1
 *         §Compatibility a monitor MUST read it that way, never as an error.
 *
 * @note Called ONLY from the cold `:stats` read path, never on a hot path. It MUST NOT
 *       re-enter `graph_t`, and @p ctx must outlive every read the graph can still serve —
 *       the lifetime the router's other five seams already require.
 */
using stats_sampler_fn_t = bool (*)(void* ctx, std::string_view seam_class,
                                    std::string_view seam_name, stats_block_t* out);

/**
 * @brief Report that ONE remote subscription edge routed through link @p link was
 *        established (@p held true) or torn down (@p held false) — RFC-0014 §4's standing
 *        binding, as the routing plane sees it (#1816).
 *
 * "Routed through" means the edge DELIVERS over the link: its stored delivery link, which
 * is the link the subscribe arrived on, or the mount a `SUBSCRIBER`'s `PATH` target routes
 * through (RFC-0021). A `:subscribers[]` field-write edge, which delivers to a LOCAL target,
 * is not routed through any link and is never reported. Each edge is reported exactly
 * twice over its life — once `true` when it is admitted, once `false` when it is cleared,
 * replaced, or evicted — so the receiver can keep a plain reference count and nothing else.
 *
 * @param ctx  The caller-owned context installed beside the function.
 * @param link This node's NAME for the link — the router's registry name. Borrowed for the
 *             call only.
 * @param held `true` on establishment, `false` on teardown.
 *
 * @note Called on the subscribing or unsubscribing thread, OUTSIDE every graph lock, after
 *       the mutation has landed. Never called on the write or delivery path. It may block
 *       briefly on the receiver's own control-plane lock; it MUST NOT re-enter `graph_t`.
 *       @p ctx must outlive every subscribe the graph can still admit.
 */
using link_hold_fn_t = void (*)(void* ctx, std::string_view link, bool held);

/**
 * @brief One `{fn, ctx}` graph seam: a captureless function pointer and the context handed
 *        back as its first argument (ADR-0047, #1049).
 *
 * The shape `receiver_slot_t` and `hook_t` already use. A null @ref fn is the uninstalled
 * seam — the graph's documented default for each one.
 */
template <class Fn>
struct graph_hook_t {
    Fn fn = nullptr;     /**< @brief The callback, or null when the seam is not installed. */
    void* ctx = nullptr; /**< @brief Handed back as @ref fn's first argument; caller-owned. */
};

/**
 * @brief The CREATION HOOK (RFC-0030 §7.2): a parent vertex's app logic that decides whether a
 *        data write to a missing child creates it.
 *
 * Called with the parent, the missing child's full canonical key (its last segment is the
 * child's name, `key_view_t{child_key}.last_segment()`), the writer's subject (empty for the
 * node's own local write) and the written payload. To create, the hook registers exactly ONE
 * vertex at `child_key` — typed, configured and policied as the application decides, through
 * the ordinary registration calls — and returns success. To refuse, it returns
 * `std::unexpected(status_t::NOT_FOUND)`, which is what the writer is told. Any other status
 * (for example `BACKPRESSURE` from a registration the graph's source refused) passes through to
 * the writer unchanged.
 *
 * The graph has already evaluated the parent's `CREATE` right for the subject before the call;
 * a denial never reaches the hook. A hook that answers success but registers nothing answers
 * the writer `NOT_FOUND`. Any bound on how many children a peer may cause is the hook's own
 * decision; the library adds none (`CONTEXT.md` §Resource bound).
 *
 * @warning All arguments are BORROWED for the call. The hook runs on the writer's thread with
 *          no graph lock held, so it may register vertices; it must not retire the parent.
 */
using creation_hook_t =
    hook_t<result_t<void>(vertex_handle_t parent, std::span<const std::byte> child_key,
                          std::string_view subject, const view::rope_t& payload)>;

/**
 * @brief The graph's five wiring seams as ONE aggregate (RFC-0028 §4.12, D12) — what
 *        `graph_t`'s constructor and @ref graph_t::set_hooks take, in place of the five
 *        `configure_*` verbs slice 10 deleted.
 *
 * Every slot is a @ref graph_hook_t, published through a `tr::sink_slot_t`, so a hot-path
 * reader dispatches from one coherent `{fn, ctx}` snapshot — never a new `fn` beside a stale
 * `ctx`. Every slot defaults to null, which is each seam's documented default, so
 * `graph_hooks_t{}` is the un-wired graph and a caller names only what it installs:
 *
 * @code
 * tr::graph::graph_t g{pool, {.subject_resolver = {&resolve, &my_acl}}};
 * @endcode
 *
 * CONFIGURATION, not runtime knobs (#1049): install at wiring time, from one thread, before
 * frames flow. Each `ctx` must outlive every dispatch that can still reach its seam.
 */
struct graph_hooks_t {
    /**
     * @brief The ACL enforcement switch (ADR-0018): maps a non-empty caller context to a
     *        subject token.
     *
     * Null (the default) DISABLES enforcement: every operation is allowed and the hot path
     * pays one relaxed load. With a resolver, each gated operation with a NON-EMPTY caller
     * evaluates the target's *effective* ACL (own ACEs + inherited, ADR-0020); denial returns
     * `PERMISSION_DENIED`. The EMPTY caller context is the local-API convention and is
     * trusted without consulting the resolver (#905), so the resolver's error arm is free to
     * mean DENY. A token equal to `tr::graph::kEveryoneSubject` is refused at every gate
     * (#908): the wire has one spelling for a subject token.
     */
    graph_hook_t<subject_resolver_fn_t> subject_resolver{};

    /**
     * @brief The EXTERNAL subscription observer — fired on every `:subscribers[]` mutation
     *        that arrived over a transport (see @ref sub_event_t for what "external" means).
     *
     * Fires from the one admission door every subscribe lands in and from the
     * `:subscribers[N]` clear. `evict_link_edges` and a local `unsubscribe` stay silent, by
     * design. Null (the default) costs one relaxed load on the subscribe path.
     */
    graph_hook_t<sub_observer_fn_t> subscription_observer{};

    /**
     * @brief The sink the producer fan-out hands each REMOTE subscriber's delivery to (#136,
     *        RFC-0004 §D/§E.1) — the transport plane's seam; `tr::net::fwd_router_t`'s
     *        constructor installs it.
     *
     * Fires on whatever thread calls `write` (outside the vertex lock), and on `subscribe`
     * for a transient-local latch. Null ⇒ remote slots are stored but never deliver.
     */
    graph_hook_t<remote_delivery_fn_t> remote_delivery{};

    /**
     * @brief The wire SUBSCRIBER target resolver (RFC-0021) — the transport plane's mount
     *        descent, borrowed by the `:subscribers[]` wire door; the router installs it.
     *
     * Null ⇒ a wire `SUBSCRIBER`'s `PATH` child is inert, every pre-RFC-0021 embedder's
     * behaviour. With one, a subscribe whose target routes through a mount binds the edge to
     * `(that mount, the residual below it)` (#491).
     */
    graph_hook_t<wire_target_fn_t> wire_target{};

    /**
     * @brief The NET-PLANE `:stats` seam sampler (RFC-0010 Amendment 2, #1503); the router
     *        installs it.
     *
     * Null ⇒ the census answers for the graph alone and every `router` / `labels` / `link`
     * spelling answers `SCHEMA_NOT_FOUND`.
     */
    graph_hook_t<stats_sampler_fn_t> stats_sampler{};

    /**
     * @brief The routed-subscription hold seam (RFC-0014 §4, #1816) — the net plane's
     *        standing-binding refcount; `tr::net::transport_vertex_t`'s constructor installs
     *        it on a build that carries the liveness engine.
     *
     * Null ⇒ subscriptions hold no link, which is every node without that engine. See
     * `%link_hold_fn_t` for when it fires.
     */
    graph_hook_t<link_hold_fn_t> link_hold{};

    /**
     * @brief The ACL enforcement switch in its caller-storage form (#1781): the same seam as
     *        @ref subject_resolver, with the token written into a buffer the gate owns
     *        (@ref subject_lookup_fn_t). Takes precedence when both are installed.
     *
     * Last in the aggregate so every existing designated initializer keeps compiling.
     */
    graph_hook_t<subject_lookup_fn_t> subject_lookup{};
};

/**
 * @brief Everything the OWNER declares about one vertex, as ONE aggregate (RFC-0028 §4.12,
 *        D12) — what `graph_t::register_vertex` and @ref graph_t::set_policy take, in place of
 *        the per-vertex `set_*` wiring verbs slice 10 deleted.
 *
 * Owner-side, host-only, with no wire surface: no peer can read or write any of it, and
 * nothing is inherited (RFC-0022 §3.F). Every member defaults to what an undeclared vertex
 * does, so `vertex_policy_t{}` is the default vertex and a caller names only what differs:
 *
 * @code
 * const auto v = g.register_vertex(path, role_t::STREAM, {},
 *                                  {.retention = retention_t::N, .depth = 64,
 *                                   .ring_source = &ring_pool, .ring_reliable = true});
 * @endcode
 *
 * A policy is stated WHOLE: @ref graph_t::set_policy applies every member, so a member left
 * at its default resets that property. Applying a member that already holds costs nothing —
 * in particular a default policy on a fresh vertex allocates no extension block.
 */
struct vertex_policy_t {
    /**
     * @brief What the vertex retains after a write is delivered (RFC-0028 §5.4, D4); unset ⇒
     *        the role's default (`STORED_VALUE` → `LAST`, `STREAM` → `N` at depth 1,
     *        `HANDLER` → `NONE`).
     *
     * Legal pairings: `STORED_VALUE` `NONE`|`LAST`, `STREAM` `NONE`|`N`, `HANDLER` `NONE`. An
     * illegal one answers `SCHEMA_NOT_FOUND` from the verb that applies the policy.
     * - `NONE` on a `STORED_VALUE` makes it a pure relay: every write is delivered and
     *   released, the write sequence still moves (so `await` wakes), `read` answers
     *   `NOT_FOUND`, and `assign` / `propagate` refuse with `SCHEMA_NOT_FOUND`. A direct write
     *   whose only subscribers are callbacks draws no block at all.
     * - `N` sets the ring depth (@ref depth); `NONE` on a `STREAM` empties and stops the ring.
     *
     * Switching to `NONE` drops what is already held. Costs zero bytes: `NONE` is a bit in the
     * vertex's flag byte, so a vertex declaring only `NONE` (no fields, no ring source, the
     * default threshold) draws no extension block — a STREAM included, at registration
     * (#2001). `N` draws the block, which holds the depth.
     */
    std::optional<retention_t> retention{};

    /** @brief Ring depth under `retention_t::N` (0 behaves as 1); ignored otherwise. */
    std::uint32_t depth = 1;

    /**
     * @brief The copy-or-share threshold in bytes (RFC-0028 §5.3, D3): a written value of at
     *        least this many bytes is SHARED, one below it is COPIED.
     *
     * At the terminus, a view-delivered, trailer-less WRITE of at least this size is stored
     * as a refcounted link to the inbound receive segment — no allocation for the bytes, no
     * copy; a smaller one is copied into the value's own block. `0` shares always; `SIZE_MAX`
     * copies always. What sharing costs on a POOLED RX backend: the shared value BORROWS a
     * pool slot until it is displaced, so size against `live shared values x segment_bytes`
     * (see `config_t::kShareThresholdBytes` for the target-class defaults).
     */
    std::size_t share_threshold_bytes = kShareThresholdBytes;

    /**
     * @brief The receiving STREAM vertex's own ring source (RFC-0025 §4.6.1 clause 3); null ⇒
     *        the graph's @ref graph_t::default_ring_source.
     *
     * A producer never queues; the queue belongs to whoever consumes it, bounded in BYTES by
     * that party's own source. Each admitted entry reserves its retained width from the
     * source until it retires. Per-injection-point, never a shared pool: one receiver running
     * its source dry must not affect another. Changing it DRAINS the ring (every reservation
     * goes back to the source that served it). Meaningful only on a `STREAM`.
     */
    mem::block_source_t* ring_source = nullptr;

    /**
     * @brief The §4.4 pressure arm for `ring_source`: `false` (default) BEST-EFFORT — a
     *        refused admission sheds the oldest entry whole, accounts the loss and raises
     *        `tr::flow::address_shift_gap`; `true` RELIABLE — the admission is refused,
     *        nothing is shed, and the LOCAL producer's write answers `BACKPRESSURE`.
     */
    bool ring_reliable = false;

    /**
     * @brief How the vertex participates in an ANCESTOR's propagate sweep (RFC-0008 §C);
     *        default `IF_NEWER`. Maintains the sweep's UNCONDITIONAL membership.
     */
    delivery_mode_t delivery_mode = delivery_mode_t::IF_NEWER;

    /**
     * @brief The vertex's application property field table (RFC-0010 §A) — owning, or
     *        BORROWED from static storage (ADR-0058); empty ⇒ no fields (the closed
     *        `ENOTTY` surface).
     *
     * Applying a policy REPLACES the table (atomically with respect to concurrent field
     * operations), unless the declaration is the very one already installed — the same
     * borrowed array, or an owning table while one of the same shape stands, in which case
     * stored field values are kept. A borrowed table AND the bytes it points at must outlive
     * the vertex.
     */
    app_fields_decl_t app_fields{};
};

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
    /**
     * @brief Construct a graph drawing **every** byte it allocates from the single
     *        injected @p src (#873 phase 1) — the collapsed successor to the four-seam
     *        constructor.
     *
     * @par What this replaced, and why
     * Until #873 phase 1 this constructor took four positional, defaulted seams —
     * a `std::pmr::memory_resource*`, a @ref mem::mem_backend_t*, and two
     * @ref mem::block_source_t* — so a deployer who wanted a bounded node had to know
     * which of four allocation channels each of its bytes travelled on, and injecting all
     * four still did not bound the process because a fifth channel (the un-injected global
     * heap) ran alongside them. ADR-0079 settled the composition and the 2026-08-26 ruling
     * settled the shape: **@ref mem::block_source_t is the substrate**, and the graph builds
     * the other two vocabularies on top of the one source it is handed. `mem_backend_t`
     * survives as a wrapper TYPE (@ref mem::source_backend_t), not as an injection seam.
     *
     * @par Which bytes flow through @p src after phase 1
     * ALL of the following, where before they came from four separately-injected places:
     * - the graph's own tables (#1778): the vertex tree, the vertex index, the link index,
     *   subscriber edge tables, the seam park, the creation catalog, the identity record, the
     *   declaration lists and the propagate-sweep sets — through the table sub-pool
     *   (@ref table_source), in core containers;
     * - the write-path copy-store's owned value @ref view::segment_t and both folded READs'
     *   exactly-sized POINT headers (ADR-0060, #831) — through an internally-built
     *   @ref mem::source_backend_t;
     * - every #551 FAILABLE allocation a peer can provoke: vertex registration, the
     *   branch-write decode's bump upstream, the composed read's collect stack
     *   (@ref control_source);
     * - the graph-level DEFAULT receiver-ring admissions of a STREAM vertex that has
     *   declared no source of its own (@ref default_ring_source);
     * - every segment the graph's READ-BACK encoders mint (#873 phase 3) — `:point`,
     *   `:settings`, `:settings.app`, `:acl`, `:children`, the identity record, the stats
     *   block, an app field's stored bytes, and the subscriber / mount-route records
     *   `subscribe` composes. These reached `tr::view::over_bytes`'s single-argument,
     *   global-heap overload until phase 3 pointed them at @ref value_backend, so a peer's
     *   READ is now charged to the deployer's slab like every other channel.
     *
     * @par Which bytes do NOT, and where that is tracked
     * Two carve-outs, both measured or documented rather than pending:
     * - the LKV **hazard-slot nodes** (`lkv_slot.hpp`) stay `new (std::nothrow)` on the
     *   global heap. #873 phase 2 built the migration, measured it, and **reverted** it:
     *   +22.7 % on hazard-node acquisition and +3.5 % on the free-list-hit steady arm, with
     *   disjoint ranges against an A/A null band of −0.12 %. The seam's own `@note` carries
     *   the figures.
     * - the read-back encoders' STAGING buffers, pinned by `tr::wire::emit_tlv`'s
     *   `std::vector<std::byte>&` sink. Phase 3 moved the resulting SEGMENT onto the
     *   injection; the transient buffer it is copied from is still the global heap's. The
     *   public signatures that still take or return owning `std` types are the same kind of
     *   residual, and move with the public-API batch of the seam migration.
     *
     * @par Failure convention
     * The substrate speaks raw `nullptr`-on-exhaustion and the graph does not wrap it.
     * The one adapter translates at its own boundary and nowhere else: the backend adapter
     * turns a refusal into a null @ref view::segment_t, which is the BACKPRESSURE signal the
     * write path already answered. The graph's own tables answer BACKPRESSURE directly. So an
     * injected @p src bounds the node, and a peer's CREATE frame can no longer reboot a
     * `-fno-exceptions` node through the failable channels.
     *
     * @par NARROW vs WIDE is WHICH source, never a config knob
     * A host that passes nothing gets @ref mem::default_root (ADR-0083 Decision 4, #1777).
     * Where `kSlabPool` is `true` that is the host root (`%mem_slab_pool.hpp`), and the graph
     * DERIVES its sub-pools from it: values (published values, ring admissions and every
     * segment @ref value_backend mints, through @ref mem::heap_backend) from the value
     * sub-pool, and registration and container blocks from the table sub-pool. The platform
     * allocator then sees whole slabs only. Where it is `false` the default is the MCU
     * static arena (`%mem_arena.hpp`, #1783), with the same three sub-pools and no heap. A bounded
     * node injects a @ref mem::pool_source_t (or a
     * @ref mem::bump_source_t over `null_source()`) and the slab's size IS the bound
     * (ADR-0079): an injected root serves every purpose itself, and no sub-pool is derived
     * from it (@ref derives_sub_pools). No `default_config_t` option expresses the bound and
     * none will: the divergence is the injected object.
     *
     * @par Per-domain overrides still exist, at the seams that own the resource
     * One injection is the DEFAULT, not a mandate that everything share a store. A STREAM
     * vertex that must not be affected by another receiver's exhaustion declares its own
     * ring source through @ref vertex_policy_t::ring_source (receiver-pays, RFC-0025 §4.6.1 clause
     * 3) — that seam is untouched, and per-vertex isolation stays a tested property.
     *
     * @warning **A `value_ref_t` must not outlive the graph it was read from.** This is the
     *          one contract the collapse tightens, and it is stated rather than discovered: a
     *          stored LKV is a `value_t` block drawn from the graph's source, so the handle's
     *          last `release` hands the block back to that source when
     *          the last reference drops. Before the collapse that resource was HOST-owned and
     *          the host could keep it alive past the graph; now it is a graph member, so a
     *          handle released after `~graph_t` calls a destroyed object. The graph's own
     *          members are safe by construction — the two adapters are declared FIRST, so they
     *          are destroyed LAST, after the vertex tree that holds every stored LKV — but a
     *          handle the application copied out is the application's to drop first. Every
     *          `vertex_handle_t` obtained from the graph already dangles at that point, so
     *          nothing in the reference API is meant to outlive it.
     *
     * @param hooks The graph's wiring seams (@ref graph_hooks_t), installed before the
     *              constructor returns. Default: none — ACL enforcement off, no observer, no
     *              transport plane. A router constructed later installs its three through
     *              @ref set_hooks.
     * @param src The one nothrow failable block source every allocation above draws from.
     *            Host-owned; it MUST outlive the graph and every value handle obtained
     *            from it. An injected source must be thread-safe on a target where a value
     *            segment's reclaim can self-route onto a reader/subscriber thread
     *            concurrent with a writer's allocation (ADR-0060 §2) — @ref mem::heap_source
     *            is; a @ref mem::pool_source_t must be composed with the target's
     *            arch-selected synchronisation. Any `mem_backend_t` is a source too
     *            (RFC-0028 slice 10), so a deployer that injects one slab has one slab.
     */
    explicit graph_t(mem::block_source_t& src = mem::default_root(), graph_hooks_t hooks = {});

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
     * @brief The graph-level DEFAULT receiver-ring source (RFC-0025 §4.6.1 clause 3).
     *
     * What a STREAM vertex charges its ring admissions against until it declares its own
     * through @ref vertex_policy_t::ring_source, is the value sub-pool (@ref value_source), since
     * #1777. Exposed for the same reason @ref control_source is: so a host can name it in a
     * memory census and so the wiring is observable.
     */
    [[nodiscard]] mem::block_source_t& default_ring_source() const noexcept { return *values_; }

    /**
     * @brief Where this graph's VALUES are drawn from (ADR-0083 Decision 3, #1777): every
     *        published value, the default ring admissions, and a router's warm COMPACT copy.
     *
     * The value sub-pool of the host root (`:stats.mem.values`) on a default graph that
     * @ref derives_sub_pools; the injected root otherwise.
     */
    [[nodiscard]] mem::block_source_t& value_source() const noexcept { return *values_; }

    /**
     * @brief Where this graph's TABLE blocks are drawn from: vertex registration, the
     *        control-plane containers and the failable scratch of a composed read or a branch
     *        write (`:stats.mem.tables`). On a default graph of a `kSlabPool` build, a table
     *        sub-pool of the graph's OWN, derived from the host root's platform heap (#1778):
     *        independent graphs share no class lock and no cache line. On the MCU arena, the
     *        arena's table sub-pool (#1783). The injected root otherwise, as @ref value_source.
     */
    [[nodiscard]] mem::block_source_t& table_source() const noexcept { return *tables_; }

    /**
     * @brief Return every fully free slab of this graph's own table sub-pool to the platform
     *        heap, on the caller's schedule (the graph keeps no timer to do it). A no-op on a
     *        graph without one (an injected root, or a build without the slab pool); the
     *        process-wide sub-pools are trimmed by `tr::mem::host_root().trim()`.
     */
    void trim_tables() noexcept;

    /**
     * @brief The NET sub-pool a router or link on this graph defaults to when the application
     *        injects nothing (ADR-0083 Q21, `:stats.mem.net`): @ref mem::net_source on a graph
     *        that @ref derives_sub_pools, the injected root otherwise.
     *
     * The router and transport sources stay separately injectable (receiver-pays); this is
     * only their default, and the census name a monitor reads it by.
     */
    [[nodiscard]] mem::block_source_t& net_source() const noexcept {
        return derives_sub_pools() ? mem::net_source() : *ctl_;
    }

    /**
     * @brief Whether this graph derived its sub-pools from the build's default root (#1777,
     *        #1783): `true` for a graph built without a source, on the host slab pool and on the
     *        MCU arena alike.
     *
     * An injected root serves every purpose itself, so its census is `:stats.mem.control`
     * alone, and `:stats.mem.values`, `.tables` and `.net` answer `SCHEMA_NOT_FOUND`
     * (RFC-0010 Amendment 3: "a node that does not derive a given sub-pool").
     */
    [[nodiscard]] bool derives_sub_pools() const noexcept { return values_ != ctl_; }

    /**
     * @brief The @ref tr::mem::mem_backend_t every @ref view::segment_t this graph owns is
     *        drawn from (#873 phase 3).
     *
     * @ref mem::heap_backend for a process-default graph, and the graph's own
     * @ref mem::source_backend_t over the injected source otherwise — the pointer the
     * constructor resolved once. Exposed for the reason @ref control_source is (census and
     * observable wiring) and because the graph's own read-back encoders, which are free
     * functions in `graph.cpp` rather than members, need to name it: every segment the graph
     * mints must come from the one injection, not from the global heap that
     * `tr::view::over_bytes`'s single-argument overload reaches.
     */
    [[nodiscard]] mem::mem_backend_t& value_backend() const noexcept { return *value_backend_; }

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
                                                  vertex_policy_t policy = {},
                                                  std::span<const payload_right_t> rights = {});

    /**
     * @brief Register a vertex at @p path — FALLIBLE (the runtime-path form of
     *        @ref register_vertex).
     * @param handlers The vertex's user seams (@ref handlers_t); every `ctx` must outlive
     *        the registration.
     * @param policy The owner's declarations about the vertex (@ref vertex_policy_t), applied
     *        before the handle is returned. A default policy costs nothing. An illegal
     *        retention for @p role answers `SCHEMA_NOT_FOUND` and registers nothing.
     * @param rights OPTIONAL payload-type → required-ACL-right table (RFC-0014 Amendment 2) —
     *        the general contract by which a control vertex demands something other than
     *        plain `WRITE` for a given written TLV type. BORROWED for the call: the rows are
     *        copied onto the graph, under the same lock that publishes the vertex, so the
     *        declaration is in force before the first write can reach it. Empty (the default)
     *        ⇒ every write gates on `acl_right_t::WRITE`, and the vertex carries not one byte
     *        for this; the write gate then stops at one relaxed flag-bit test on a word it
     *        already holds. A row whose `%payload_right_t::type` equals the written value's
     *        leading TLV type supplies the right demanded instead; an unmatched type (and a
     *        value whose leading link cannot be read, e.g. a device-memory link) falls back to
     *        `WRITE`. Rows are scanned in order, first match wins. The refusal is still the
     *        ONE write gate's, counted into `delivery_drops_t::denied`: this declaration
     *        changes WHICH right is demanded, never where the demand is made. (It was
     *        `handlers_t::payload_rights` until RFC-0028 slice 7 took it out of the seam
     *        struct.)
     * @return The pinned @ref vertex_handle_t, or `PATH_IN_USE` if the path is already
     *         registered.
     */
    [[nodiscard]] result_t<vertex_handle_t> try_register_vertex(
        const path_t& path, role_t role, handlers_t handlers = {}, vertex_policy_t policy = {},
        std::span<const payload_right_t> rights = {});

    /**
     * @brief Register a vertex by its canonical PATH-payload @p key directly (the in-band
     *        `:children[]` path) — FALLIBLE.
     *
     * The key is a composed parent-key + `NAME(child)`, not parsed from a string. This is the
     * genuine runtime path (a `:children[]` write can race a duplicate name), so it stays
     * fallible. @p handlers, @p policy and @p rights are exactly @ref try_register_vertex's.
     * @param schema_catalog OPTIONAL content of the vertex's `:schema` `SETTINGS` — the catalog
     *        a CONTROL vertex declares for what its writes accept (RFC-0014 Amendment 3: the
     *        creator endpoint's `POINT{NAME, SETTINGS{…catalog…}}`). The bytes are a sequence of
     *        encoded TLVs, served VERBATIM and never parsed here; the declaring caller owns their
     *        vocabulary and validates writes against it itself. BORROWED for the call: copied
     *        onto the graph beside @p rights, under the same lock and in the same immortal node,
     *        so it is in force before the vertex is reachable. Empty (the default) ⇒ the
     *        ordinary empty `SETTINGS`, and a vertex that declares no catalog carries nothing.
     * @return The pinned @ref vertex_handle_t, or `PATH_IN_USE` if the key is already registered.
     */
    [[nodiscard]] result_t<vertex_handle_t> register_vertex_key(
        std::span<const std::byte> key, role_t role, handlers_t handlers = {},
        vertex_policy_t policy = {}, std::span<const payload_right_t> rights = {},
        std::span<const std::byte> schema_catalog = {});

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
     * owner-side storage declarations, delivery mode and creation hook, so a later
     * registration that revives the same address
     * inherits **nothing** of the retired owner — in particular the revived path inherits
     * its live ancestor's ACL policy, never the retired one's (the §Discussion-7 ruling:
     * an ACL does not survive churn). `write_seq_` survives (forward-only per address,
     * modulo 2^32).
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
     * @brief This vertex's OWN delivering subscriber-slot count (#635) — how many slots a
     *        delivery here would feed, for sizing and observability. A suspended slot
     *        (@ref set_suspended) is not counted (#1533).
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

    /** @brief @ref set_vertex_ceiling's "no ceiling" value, and its default. */
    static constexpr std::size_t kNoVertexCeiling = static_cast<std::size_t>(-1);

    /**
     * @brief Cap the node's vertex population at @p max_vertices allocations, charged
     *        against the @ref vertex_slot_count census (#1314).
     *
     * The census already counts every `vertex_t` this graph ever allocated — including the
     * placeholders a descent materializes and the landing sites an RFC-0005 §D branch write
     * decomposes into. What it did not do was *charge* anything: every creation door
     * (registration, then the write-create `mkdir -p` and branch-write decomposition, which
     * RFC-0030 §7 replaced with a parent's creation hook) allocated until
     * the allocator itself refused. A branch writer that is already resolved and already
     * WRITE-gated is therefore governed — every landing site passes its CREATE/WRITE gate —
     * but its landing sites cost nothing, so "more writes, wider writes" is an unbounded
     * vertex population multiplied by a peer's choice. That is the peer/writer-multiplied
     * allocation class
     * [ADR-0079](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)
     * fences elsewhere: a per-call bound a caller can multiply is not a node bound.
     *
     * This is the node bound, and it is the #838 shape — **count, then act** — over the census
     * that already exists rather than a second, bespoke counter. Past the ceiling every
     * creation door answers `BACKPRESSURE`, the same exhaustion status an injected
     * @ref tr::mem::block_source_t answers with, so a caller that already handles a refusing
     * store needs no new vocabulary. Refusals are counted (@ref vertex_ceiling_refusals) so a
     * node can see the bound bite instead of inferring it from a failed write.
     *
     * **Policy stays with the deployer**, per ADR-0079 §Decision 4: the default is
     * @ref kNoVertexCeiling, so an un-sized node behaves exactly as before and the library
     * fixes no synthetic limit. When ADR-0079's stage-2 graph placement store lands and
     * `vertex_t` itself draws from the injected `ctl` seam, the store's size becomes the
     * natural bound and this ceiling becomes the coarse-grained backstop rather than the
     * primary one.
     *
     * @note The census is append-only (retirement revives in place, it does not free), so the
     *       ceiling is a high-water mark on ALLOCATIONS, not a live occupancy that a retire
     *       gives back. That matches what it is bounding — memory a peer made this node
     *       commit — and it is why no release path is needed.
     * @note A refusal mid-descent leaves the levels already created in place, exactly like a
     *       refusal partway down a chain of creation hooks (each level a hook created stays).
     *       The bound holds regardless: those levels are themselves charged.
     * @note Session identity anchors are NOT charged here. They take a census slot but are
     *       created through @ref register_session_anchor, which is already bounded by the
     *       listener's `max_peers` accept policy; charging them twice would let graph growth
     *       refuse a session admission the accept policy had already granted.
     */
    void set_vertex_ceiling(std::size_t max_vertices) noexcept;

    /** @brief The ceiling in force (@ref kNoVertexCeiling when unset). */
    [[nodiscard]] std::size_t vertex_ceiling() const noexcept;

    /** @brief How many creations the ceiling has refused since construction (monotonic). */
    [[nodiscard]] std::uint64_t vertex_ceiling_refusals() const noexcept;

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
     * structural root that `roots_` cannot reach, so:
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
     * @note This is a **control-plane** call, but it is no longer priced as a scan (#1486).
     *       The reverse direction IS memoized: every vertex carries the index of its own
     *       slot, stamped once at slot assignment under the same unique hold that appended
     *       it, and this call validates that memo against the index and returns. The scan
     *       survives only as the fallback for a `vertex_t` no graph slotted.
     *
     *       The memo was previously declined on a footprint argument — "4 bytes on rv32,
     *       where `sizeof(vertex_t)` sits at its ratchet with zero headroom" — and the #1487
     *       census falsified its premise. There are 4 dead bytes at object offset 36–39 on
     *       BOTH ABIs, and spending them costs zero: `sizeof(vertex_t)` stays 96 / 72 and
     *       both `config_t` ratchets still pass, pinned to their measurements. The price is
     *       a layering compromise, not RAM — the bytes are only reachable from inside
     *       `path_key_t` (they are `name_`'s tail padding on x86-64), so the memo lives
     *       there, documented as borrowed. A pointer→index side map, the other candidate,
     *       would still cost strictly more than the 4 B/vertex RFC-0024 §6.4 priced.
     *
     *       What made it worth spending is that the scan was O(N) **under the shared
     *       `map_mutex_`** — 450 ns at 10³ resident vertices and 410 µs at 10⁶
     *       (#1485/#1496) — so route formation over M bindings paid O(M×N) and every
     *       concurrent reader queued behind the hold. The mint is once per binding, but
     *       "once per binding" is M times, not once.
     *
     * @warning The memo is INTERNAL. It is not exposed on `path_key_t`, is not exposed here,
     *          and is not a second staleness signal: the generation remains the whole of
     *          that (RFC-0024 §5.1). The hot path — @ref deref_vertex_slot — pays a bounds
     *          check and one compare and never comes here.
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
     * @brief True iff this graph enforces an ACL at all — a subject hook is installed
     *        (`subject_lookup`, or `subject_resolver` through its adapter).
     *
     * One relaxed load of the slot the ACL gate reads. With no hook every @ref allows answers true
     * for a remote caller, so a hop that would first have to LOCATE the vertex to evaluate at (a
     * NAME-spelled forward hop) asks this first and skips the lookup on a node that enforces
     * nothing. A hint, like `sink_slot_t::installed`: a resolver installed concurrently is observed
     * by the next frame.
     */
    [[nodiscard]] bool acl_enforced() const noexcept { return subject_lookup_.installed(); }

    /**
     * @brief Free every value seam @ref retire parked, and run every release @ref park_release
     *        parked, that no router frame can still reach — the EXPLICIT collector (#576).
     *
     * @ref retire detaches a vertex's value seam and **parks** it: the seam is read
     * lock-free, so the retiring thread cannot free the block a concurrent reader may
     * still be dereferencing. Parking alone has no other end, so a node that retires
     * seam-bearing vertices repeatedly grows the park forever. This is that other end, and
     * it is the embedder's call, not the library's.
     *
     * **What it waits for: every router frame open at park time.** Each park closes an epoch
     * of the QSBR domain (`%detail_qsbr::advance`) after its unpublish, and `fwd_router_t`
     * brackets every inbound frame and every origination with that domain's read side, in
     * every build. An entry is freed only once every bracket open at its epoch has closed, so a
     * frame still inside a removed link, or inside a retired seam it reached over the wire,
     * keeps it alive however many times this runs, and the first call after the frame leaves
     * frees it. An entry that is not yet ripe stays parked for a later call.
     *
     * **Which vertices park — handler PRESENCE, never role.** `vertex_t::adopt_identity`
     * allocates the `value_handlers_t` iff at least one of `on_read`, `on_write`,
     * `on_children` was installed at registration; `role_t` is never consulted. So a
     * `role_t::STORED_VALUE` vertex registered with an `on_children` parks one seam on
     * retirement, and a `role_t::HANDLER` registered with an empty @ref handlers_t parks
     * nothing. Scoping a quiescent point by role excludes exactly the production case
     * below.
     *
     * **The transport plane's append sites.**
     * `tr::net::transport_vertex_t::remove_connection` retires the `/net/<module>/<name>`
     * identity vertex, which is registered `role_t::STORED_VALUE` — and it bears a seam
     * only when its link exposes a bus facet (`transport_t::bus() != nullptr`): the CAN
     * binding, and a tcp/ws server wired `peer_named = true`, get an `on_children` that
     * synthesizes the live peer listing (ADR-0044). That seam is peer-driven: a bus node
     * parks one `value_handlers_t` per connection teardown, and a point-to-point teardown
     * (a dial link, UDP, loopback, a default-wired server) parks no seam. EVERY connection
     * teardown parks one @ref park_release, though: the removed link, shut down, which this
     * call destroys. Destroying a `tr::net::transport_vertex_t` also parks one seam per
     * declared module's `<module>/conn` creator endpoint, plus one release for that
     * endpoint's context. So a node whose connections come and go needs a collect point, or
     * the park grows by that much per teardown until the graph goes; until it runs, a
     * removed link holds its object (and, over lwIP, its socket).
     *
     * @warning **In-process readers are still the caller's to settle.** A router frame never
     *          needs a quiet point, but a `read` / `write` / `:children[]` an application
     *          thread makes on the graph directly is not bracketed, and it holds the raw seam
     *          pointer across the user callback it invokes. So call this where no such call
     *          is in flight on another thread: on a single-threaded node any point between
     *          operations, on a threaded node the one thread that runs the application's
     *          graph operations. The hazard is NOT limited to a thread that started on an
     *          already-retired vertex: those calls load the seam pointer ONCE (deliberately
     *          — a second load could see a concurrent retire's null), so a thread that
     *          entered while the vertex was still **LIVE** holds that raw pointer across the
     *          whole user callback, and a retire landing mid-callback moves the block it is
     *          using into the park.
     *
     * The free runs on the CALLER's thread and OUTSIDE every graph lock: the ripe entries
     * are taken in stack-sized batches under the map lock, which draws nothing, and freed
     * after it is released. So a release may re-enter the graph (drop a handle, `find` a
     * path, retire something else) without deadlocking, and an arbitrarily slow one blocks
     * no reader or writer. The seams are freed before the releases of the same batch.
     *
     * A no-op when nothing is parked or nothing is ripe. An embedder that never calls it
     * keeps the pre-#576 behaviour — the park grows without bound — which @ref
     * parked_seam_count makes observable. Called from inside a router frame, it frees
     * nothing that frame could reach: the calling thread is itself still inside a bracket.
     *
     * @note Whatever is still parked when the graph is destroyed is freed by the graph's
     *       own teardown, ripe or not — a backstop against unbounded growth, NOT a substitute
     *       for this call; by then the embedder has stopped every frame. `retired_seams_`
     *       is declared before `map_mutex_` and `roots_`, so it destructs **last**, after the
     *       vertex tree and the map lock are already gone: a seam callback whose destructor
     *       re-enters the graph re-enters a half-destroyed object and crashes. Such an owner
     *       is safe HERE and only here — it must be collected explicitly, never left to
     *       teardown.
     */
    void collect();

    /**
     * @brief Hand the graph a retired seam's context to release at the first @ref collect
     *        that no router frame open now can still be inside.
     *
     * The other half of @ref retire for a seam owner that is going away. Retiring stops new
     * calls through the seam, but a reader that loaded it before the retire may still be on
     * its way into the callback, holding the context the owner would free. So an owner that
     * unpublishes what it is about to free parks the free here instead: this call closes an
     * epoch after the caller's unpublish, and @p release runs exactly once,
     * `release.release(release.ctx)`, on the thread of the first @ref collect call made after
     * every router frame open at that epoch has left, outside every graph lock, after the
     * seams that call frees. A release still parked when the graph is destroyed runs in its
     * teardown, with the same caveat as a parked seam there: it must not re-enter the graph.
     *
     * The callback is the ADR-0080 @ref retired_callback_t, the shape `unsubscribe` hands a
     * retired subscription's context back through. Whatever reaches the context in the
     * meantime is the owner's to answer; it stays valid until the release runs.
     *
     * @retval BACKPRESSURE The park could not grow; nothing is parked and @p release will
     *         never run, so the context must stay valid for as long as the graph lives.
     */
    [[nodiscard]] result_t<void> park_release(retired_callback_t release);

    /**
     * @brief How many retired value seams are currently parked, awaiting @ref collect.
     *
     * The observability half of the collector: an embedder that never calls @ref collect
     * has a number it can watch (a health field, an assert in a soak test) instead of a
     * silent, peer-driven leak. Grows by one per retired vertex that BORE a value seam —
     * i.e. one that had any of `on_read` / `on_write` / `on_children` installed at
     * registration, whatever its `role_t` — and drops to zero on a @ref collect that no
     * router frame open at their retirement is still inside. A
     * retired vertex with no value seam parks nothing, including a `role_t::HANDLER` one
     * registered with an empty @ref handlers_t. On the transport plane that means one per
     * `/net/<module>/<name>` identity vertex whose link exposes a bus facet (CAN, or a
     * tcp/ws server wired `peer_named = true`) and **zero** for every point-to-point
     * connection, plus one per creator endpoint of each `tr::net::transport_vertex_t`
     * destroyed over this graph — so on a default deployment that never destroys its
     * transport vertex this legitimately never leaves 0.
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
     * `%link_index_t`), so this can exceed the number of edges an eviction would actually
     * reclaim. It is an upper bound on work, which is exactly what the scaling property is
     * about — never read it as "how many edges this link has".
     * @param link_name This node's NAME for the link; empty ⇒ 0 (a local edge is not
     *                  reachable by link teardown).
     * @return The number of candidate vertices, i.e. the departure's bounded cost.
     */
    [[nodiscard]] std::size_t link_edge_candidates(std::string_view link_name) const;

    /**
     * @brief Mint-or-find @p link_name's interned token — the LINK-UP door (#1266 / #1417).
     *
     * The transport plane calls this ONCE, when a link or a bus peer becomes audible, caches
     * the answer in its own per-link receive context, and hands it to every subsequent
     * subscribe through `op_resolver_t::on_link_id`. That is the whole of the carry, and it
     * is what takes the index operation from 14.1–17.2 ns to 9.4 ns — FLAT in link count —
     * and its footprint from 175.8 to 128.0 bytes per link (measured on #1416's harness,
     * `bench/bench_subscribe_index`, 8 vertices, best-of-13-rounds against a carried A/A null).
     *
     * IDEMPOTENT BY NAME, which is the property #1263 pinned and this must not move: the
     * same spelling always answers the same live token, so a redialing peer that comes back
     * under its old name re-enters its old slot rather than stranding it. A name that has
     * never been interned takes a fresh slot (or a released one), and the returned token is
     * valid until @ref release_link or a whole-link eviction retires that slot.
     *
     * @param link_name This node's NAME for the link; empty ⇒ an invalid token (the #1056
     *                  empty-key rule — the LOCAL spelling is not a link).
     * @return The token, or a default-constructed one for an empty name.
     */
    [[nodiscard]] link_id_t intern_link(std::string_view link_name);

    /**
     * @brief @ref intern_link with a caller-held SLOT HINT — the O(1) door for a caller that
     *        has somewhere stable to keep one word (#1437).
     *
     * @ref intern_link's mint-or-find goes through the name door, and that door is a LINEAR
     * SCAN of the live slots (`%link_index_t`'s "DENSE SLOT VECTOR" block spells out why —
     * deliberately NOT an `@ref`, because that anchor lives on an implementation type the
     * rendered API reference does not emit, and a link to it is a `-n -W` sphinx failure).
     * Cheaper than the hash it
     * replaced up to about 32 live links and about 2x dearer at 65 — which is fine on the
     * paths the trade was argued on (once per peer hangup) and NOT fine on the un-carried
     * subscribe doors, which pay it per subscribe. This is those doors' way out: a caller
     * that owns a durable word per link (a registered mount, say) keeps the slot number in
     * it and gets the whole find for one bounds check and one name compare.
     *
     * The hint is a PURE CACHE with NO invalidation contract, which is the property that
     * makes it safe to park in a structure nobody synchronizes: a stale hint — a slot
     * released and re-minted under another name, a hint from a different graph, an
     * uninitialized zero — fails the name compare and costs one wasted comparison before the
     * scan it would have paid anyway. Nothing a wrong hint can spell produces a wrong token,
     * so the caller owes this word no upkeep on link-down, on re-add, or on teardown.
     *
     * The generation is NOT part of the hint and must not become part of it. The slot's name
     * IS the validation: a live slot's spelling is unique across the index (@ref intern_link
     * is mint-or-find by name) and a dead slot spells itself empty, so a name match already
     * proves the slot is live, is this link's, and carries the stamp that is current right
     * now — read from the slot under the same lock rather than remembered by the caller.
     *
     * @param link_name This node's NAME for the link; empty ⇒ an invalid token, hint
     *                  untouched (the #1056 empty-key rule).
     * @param hint      In: where this name was last seen. Out: where it is now, on any
     *                  non-empty name that interned — so the next call is the fast path.
     * @return Exactly what @ref intern_link would answer for @p link_name.
     */
    [[nodiscard]] link_id_t intern_link_hinted(std::string_view link_name, std::uint32_t& hint);

    /**
     * @brief Retire @p token's slot so it can be reused — the LINK-DOWN half of @ref
     *        intern_link.
     *
     * Bumps the slot's stamp, so every copy of @p token still in flight stops validating and
     * degrades to a name lookup rather than addressing whatever link takes the slot next.
     * The candidate list is released with it: this is the teardown door, so anything still
     * listed is by definition an edge that departed with the link.
     *
     * Optional in the safety sense and NOT in the footprint sense: a node that never calls it
     * keeps one 64-byte slot per distinct link name it has ever seen, where one that does
     * keeps one per link it currently holds. `evict_link_edges` already releases the slot it
     * empties, so the transport plane's ordinary hangup path needs no extra call; this exists
     * for an owner tearing a link down without evicting through the graph.
     *
     * A token that is invalid, out of range, or already released is a no-op.
     */
    void release_link(link_id_t token);

    /**
     * @brief How many index inserts had to fall back to a NAME LOOKUP — the carry's own
     *        observability (#1417).
     *
     * A carried token that validates costs a subscript; anything else costs a scan of the
     * live slots. This counts the second case, and it is the ONLY way to see from outside
     * whether the carry is actually working: a valid token and a name lookup produce
     * byte-identical index state by construction, which is what makes the carry safe and
     * also what makes it invisible.
     *
     * Expected to be SMALL and bounded, not zero. It counts one per link that has never been
     * interned (the transport plane's lazy mint goes through `intern_link`, so that is not
     * counted), one per admission whose key is not the arrival link — a mount-routed target,
     * a `field_write` `caller` fallback — and one per `subscribe_wire` reached with no
     * transport plane behind it, which is every host-side caller and every test that binds an
     * edge directly. The purely LOCAL `subscribe()` doors never reach here at all: they admit
     * no `remote`, so there is no link to index them under. A count that GROWS with traffic on
     * a steady link set means the carry is not reaching the index, which is a performance
     * defect, never a correctness one.
     */
    [[nodiscard]] std::size_t link_index_name_lookups() const;

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
     * `unconditional_`, byte-keyed sorted tables), and it earns its keep the same way: the
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
     *          registered vertex plus the snapshot array, all from the graph's table source,
     *          then sorts. Do not put this on a delivery or write path.
     *
     * @return false when the table source refused the snapshot (#1778): @p fn was called for
     *         nothing. A walk either visits every vertex or none.
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
    bool for_each_vertex(Fn&& fn) const {
        struct entry_t {
            mem::bytes_t key; /**< @brief The vertex's canonical key. */
            vertex_t* v;      /**< @brief The vertex. */
        };
        mem::block_array_t<entry_t> snap(*tables_);
        bool ok = true;
        {
            const std::shared_lock lock(map_mutex_);
            vertex_t* const r = root();
            const auto take = [this, &snap, &ok](vertex_t& c) {
                if (!ok || !c.registered()) return;
                mem::bytes_t k(*tables_);
                ok = try_build_key(&c, k) && snap.push_back(entry_t{std::move(k), &c});
            };
            take(*r);
            r->for_each_descendant(take);
        }
        if (!ok) return false;
        std::sort(snap.begin(), snap.end(), [](const entry_t& a, const entry_t& b) {
            return mem::bytes_less_t{}(a.key, b.key);
        });
        for (const entry_t& e : snap)
            fn(wire::key_view_t{mem::as_span(e.key)}, vertex_handle_t{e.v});
        return true;
    }

    /**
     * @brief A child-vertex factory: the device-catalog entry ADR-0017 makes concrete.
     *
     * Given the composed child key (parent key + the SPEC's `name` NAME) and the optional
     * SPEC `config` SETTINGS (a node read in place over the SPEC's bytes, valid for the call
     * only — #1829), it registers the child vertex(es) and returns the primary
     * handle (or a status — e.g. `PATH_IN_USE`). The graph owns the *addressing* (the key
     * is composed for it); the factory owns the *catalog* (what a `type` instantiates).
     * A @ref hook_t (RFC-0028 D10): its `ctx` must outlive the graph.
     */
    using child_factory_t = hook_t<result_t<vertex_handle_t>(
        graph_t&, std::vector<std::byte> child_key, const wire::tlv_node_t* config)>;

    /**
     * @brief Populate the device creation catalog (ADR-0017): map a SPEC `type` selector
     *        to a @ref child_factory_t.
     *
     * A `:children[]` SPEC write whose `type` is unregistered returns `SCHEMA_NOT_FOUND`
     * (the ENOTTY of an unsupported creation). The built-in `stored_value` type is
     * registered by the constructor.
     *
     * CONFIGURATION, like the `graph_hooks_t` seams: populate the catalog at setup,
     * before frames flow. Unlike them the catalog is a sorted table, so #1049's `{fn, ctx}`
     * publication does not reach it — a concurrent insert moves entries the in-band
     * creation path may be reading. Registration and lookup therefore take a lock, which
     * costs nothing: both are control-plane cold (one lookup per created vertex) and
     * neither is on a read, write or dispatch path. Violating the setup-only contract is
     * consequently slow rather than corrupting.
     *
     * The entry draws from the graph's table source (#1778). Setup is where the catalog is
     * sized, so a refusal here is a sizing bug: it aborts with a message naming the source
     * and the bytes it was asked for (ADR-0056, ADR-0083).
     */
    void register_child_type(std::string_view type, child_factory_t factory);

    /**
     * @brief Read a resolved vertex's stored value (the hot path — lock-free in the LKV slot).
     *
     * Returns the last-known-value as a @ref value_ref_t (RFC-0028 D11, one read type): a
     * reference to the published block, not a copy. A scalar is the single-link case; a
     * consumer needing contiguous bytes calls `value_t::only()` (single-link, zero copy) or
     * `value_t::materialize()`, and one that needs a `rope_t` clones it with `value_t::rope()`. The
     * trailing @p caller is the ACL caller context
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
     *
     * @p link is the transport-catalog `(kind, role)` of the link the write arrived on, or
     * null for a write that arrived over none (#1650). It gates nothing here: it reaches the
     * vertex's admission filter and handler as `write_ctx_t::link`, beside @p caller as
     * `write_ctx_t::subject`. The router passes it for a write a link carried; an API write
     * leaves it null.
     */
    [[nodiscard]] result_t<void> write(vertex_handle_t v, view::rope_t value,
                                       std::string_view caller = {},
                                       const net::link_kind_t* link = nullptr);
    /**
     * @brief Field-write by handle: resolve the @ref vertex_handle_t and @ref field_path_t
     *        once, then reuse them on the hot path — no string parse, no map lookup per call.
     *
     * An empty @p field is an ordinary value write. Pass `path.field()` for the field
     * selector. A field write targets a contiguous control TLV, so a multi-link value is
     * materialized first. @p link is as for the plain overload: an empty @p field hands it to
     * `handlers_t::on_admit` and `on_write`, and a `:settings.app.<name>` field write hands it,
     * with @p caller, to `handlers_t::on_app_field_admit` in the same `write_ctx_t` (#1832).
     */
    [[nodiscard]] result_t<void> write(vertex_handle_t v, const field_path_t& field,
                                       view::rope_t value, std::string_view caller = {},
                                       const net::link_kind_t* link = nullptr);
    /**
     * @brief Assign a vertex's value — the STATE transition only, sends NOTHING (RFC-0008).
     *
     * One of the two irreducible operations `write` composes: swap v's last-known-value
     * (atomic), append to the stream ring, bump the write sequence (waking await), and
     * mark v for the next covering @ref propagate sweep (unless v is EXPLICIT, or nobody
     * observes at/above it). WRITE-gated like @ref write; never gated by delivery_mode. A
     * branch POINT decomposes and assigns each descendant (no notify). Pair with
     * @ref propagate for the "update many, propagate once" workflow.
     *
     * @retval SCHEMA_NOT_FOUND @p v's role RETAINS NOTHING (a `HANDLER`), so the state half has
     *         nowhere to land and the covering sweep — which takes no value argument and reads
     *         the last-known-value — would deliver silence (RFC-0008 Amendment 2). Checked
     *         after the WRITE gate. Use @ref write instead: it dispatches the `on_write` seam
     *         and delivers eagerly, which is what a non-retaining vertex can actually do.
     */
    [[nodiscard]] result_t<void> assign(vertex_handle_t v, view::rope_t value,
                                        std::string_view caller = {});
    /**
     * @brief Propagate along subscription edges — the EDGE transition only (RFC-0008 §B/§C).
     *
     * Delivers v's current value (always — @p v is the explicit target, so a direct
     * propagate is never gated by v's delivery_mode) AND the qualifying descendants of v's
     * subtree per each descendant's delivery_mode: IF_NEWER descendants assigned since the
     * last covering sweep, and every UNCONDITIONAL descendant. Reads the last-known-value
     * — no value argument. Costs O((pending + unconditional)-in-subtree).
     *
     * @retval SCHEMA_NOT_FOUND The sweep ROOT retains nothing (a `HANDLER`) — @ref assign's
     *         refusal, at the other half of the pair (RFC-0008 Amendment 2). Only the root is
     *         judged: a sweep rooted at a retaining ancestor still walks a subtree containing
     *         non-retaining vertices exactly as before.
     */
    [[nodiscard]] result_t<void> propagate(vertex_handle_t v);
    /**
     * @brief Propagate with an explicit EMISSION MODE (RFC-0025 §4.1.2, Amendment 3 clause 5).
     *
     * Selection is identical to @ref propagate(vertex_handle_t) in both modes — same
     * `delivery_mode` gating, same subtree, same drained pending marks. Only the FRAMING
     * differs, and `emission_mode_t::PER_VERTEX` is exactly the one-arg overload, so the
     * shipped default moves under nobody.
     *
     * Under `emission_mode_t::FOLD` the sweep emits **one branch-write frame** for the swept
     * subtree instead of RFC-0008 §D's one `FWD{WRITE}` per selected vertex: the RFC-0016
     * `POINT` tree of the selection, node shape byte-for-byte RFC-0005 §B's (leading `NAME`,
     * optional `VALUE`, recursive `POINT` sub-branches), the root carrying its own leading
     * `NAME` per §B — the one root asymmetry RFC-0016 §A names between a composed-*read* root
     * and a branch-*write* root. Interior vertices that were not themselves selected appear as
     * value-free skeleton nodes so the tree stays connected; §B calls that a valid no-op node.
     *
     * The TERMINUS is untouched. RFC-0005 §B's branch-write slicing already hands each covered
     * subscription point the smallest subview covering every value at-or-below it, so a folded
     * frame needs no new decode path — and this door emits nothing a §B decomposer would
     * refuse. It is ONE FRAME PER SUBTREE, never a container across several: two disjoint
     * subtrees are two calls and two frames (the retired-LIST ban, RFC-0005 §E / ADR-0003).
     *
     * REFUSALS, all before anything is delivered or any pending mark is drained, so a refused
     * fold leaves the sweep exactly as it found it and the caller may retry `PER_VERTEX`:
     * - `TYPE_MISMATCH` — a selected vertex's stored value is not a single trailer-less `VALUE`
     *   TLV. Trailer-carrying nodes are REJECTED rather than silently stripped (§B strictness);
     *   this is admissible only because RFC-0025 Amendment 1 moved sample time out of the
     *   trailer into payload `TIME` children. A selected STREAM vertex refuses here too: its
     *   since-flush LIST cannot ride a §B node, which admits at most one `VALUE`.
     * - `BACKPRESSURE` — the fold could not be framed within the injected memory seams.
     * - `SCHEMA_NOT_FOUND` — the root retains nothing; the VERB's refusal, shared with the
     *   one-arg overload, so both modes answer alike for the same root.
     *
     * @param v    The sweep root; always delivered, never gated by its own `delivery_mode`.
     * @param mode The emission mode.
     */
    [[nodiscard]] result_t<void> propagate(vertex_handle_t v, emission_mode_t mode);
    /** @brief What @p v retains (RFC-0028 §5.4): its role's default unless its
     *         @ref vertex_policy_t declared otherwise. */
    [[nodiscard]] retention_t retention(vertex_handle_t v) const noexcept;

    /**
     * @brief Apply @p policy to @p v WHOLE (RFC-0028 §4.12, D12) — the one owner-side wiring
     *        verb that replaced `set_retention`, `set_share_threshold_bytes`,
     *        `set_ring_source`, `set_delivery_mode`, `set_app_fields` and
     *        `set_app_fields_static`.
     *
     * Every member of @ref vertex_policy_t is applied, so a member left at its default resets
     * that property; a member that already holds is skipped, so re-applying a policy costs
     * nothing and a default policy on a fresh vertex allocates nothing. A wiring-time call,
     * like `register_vertex` (the "configure before frames flow" contract): changing the ring
     * source drains the ring, switching retention to `NONE` drops what is held, and
     * re-declaring an owning field table resets its values to the declared ones.
     *
     * @return `SCHEMA_NOT_FOUND` when the policy's retention is illegal for @p v's role (a
     *         `HANDLER` asked to retain, a `STORED_VALUE` asked for a ring, a `STREAM` asked
     *         for `LAST`) — checked before anything is applied, so a refused policy changes
     *         nothing. `BACKPRESSURE` when the table source refused a block the policy needs;
     *         the policy is all-or-nothing (#1883), so this too changes no member.
     */
    [[nodiscard]] result_t<void> set_policy(vertex_handle_t v, vertex_policy_t policy);
    /** @brief Bytes @p v's receiver ring currently holds RESERVED against its source — the
     *         byte bound's observable. `SCHEMA_NOT_FOUND` on a non-STREAM role, matching
     *         @ref history. */
    [[nodiscard]] result_t<std::size_t> ring_reserved_bytes(vertex_handle_t v) const;
    /** @brief Shed points on @p v's receiver ring since registration — the cumulative
     *         `tr::flow::address_shift_gap` census (RFC-0025 §4.4: a shed with no accounting is
     *         non-conforming). `SCHEMA_NOT_FOUND` on a non-STREAM role. */
    [[nodiscard]] result_t<std::uint64_t> stream_gaps(vertex_handle_t v) const;
    /**
     * @brief Block until the vertex's value changes or @p timeout elapses; return the value.
     *
     * The READINESS FORM OF A DATA READ (RFC-0008 §A: `await` observes `assign`s at its own
     * vertex, in the state plane, independent of propagation) — so after a wake the value is
     * served through the SAME ROLE DISPATCH @ref read runs, and the two doors answer alike at
     * the same instant. A `HANDLER` vertex therefore answers from its `on_read` seam
     * (RFC-0008 Amendment 2, correcting a wake that used to answer `NOT_FOUND` *after* the
     * awaited write landed); a handler exposing no `on_read` still answers `NOT_FOUND`, which
     * is the read contract's own degradation, not await's.
     *
     * The BRANCH fork @ref read takes is deliberately not mirrored: await watches this
     * vertex's own write sequence, so a branch vertex hands back its own last-known-value, not
     * the composed subtree fold. The READ gate is checked BEFORE the wait, so a denied caller
     * cannot camp on the condition variable.
     *
     * @return The value, or a `status_t` (`TIMEOUT`, `PERMISSION_DENIED`, `NOT_FOUND`).
     */
    [[nodiscard]] result_t<value_ref_t> await(vertex_handle_t v, std::chrono::nanoseconds timeout,
                                              std::string_view caller = {});
    /**
     * @brief The non-blocking form of @ref await (ADR-0084): gate @p caller for READ, then arm
     *        the one-shot waiter @p w on @p v and return at once.
     *
     * @p w fires on the next change of @p v, on the writer's thread, after the publish has
     * landed; the callee then serves the value with @ref await_value. The graph allocates
     * nothing: @p w is the caller's, and must stay alive until it fires or @ref disarm_await
     * returns true for it. There is no deadline here — a timeout is the caller's to run, by
     * calling @ref disarm_await.
     * @return `PERMISSION_DENIED` when the READ gate refuses (nothing is armed).
     */
    [[nodiscard]] result_t<void> arm_await(vertex_handle_t v, await_waiter_t& w,
                                           std::string_view caller = {});
    /**
     * @brief Take back an armed waiter that has not fired (a timeout or a teardown).
     * @retval true  It had not fired, and it never will: it is the caller's again.
     * @retval false It already fired (or is firing) on a writer's thread.
     */
    [[nodiscard]] static bool disarm_await(await_waiter_t& w) noexcept;
    /**
     * @brief Serve a woken await's value: the role dispatch @ref await runs after its wake,
     *        shared so the blocking and the armed forms answer alike.
     *
     * The READ gate is the caller's to have checked (@ref arm_await checks it).
     * @return The value, or `NOT_FOUND` (never assigned / a handler with no `on_read`).
     */
    [[nodiscard]] result_t<value_ref_t> await_value(vertex_handle_t v) const;
    /**
     * @brief Field-read by handle (the read dual of the field-write overload).
     *
     * An empty @p field is an ordinary value read — the SAME reference @ref read hands back,
     * no copy; otherwise serve `:schema`, `:acl`, `:children[]` (the folded listing) or a
     * single `:subscribers[N]` slot (the slot's stored SUBSCRIBER view, zero-copy). For the
     * whole-array `:subscribers[]` read use @ref read_subscribers. Used by the FWD resolver.
     *
     * One read type (RFC-0028 D11): a field answer is a @ref value_ref_t like every other
     * read. A field value is COMPOSED — nothing published it — so it costs the one block
     * `value_ref_t::composed` draws from the heap; an empty @p field costs nothing.
     */
    [[nodiscard]] result_t<value_ref_t> read(vertex_handle_t v, const field_path_t& field,
                                             std::string_view caller = {}) const;
    /**
     * @brief Read the `:subscribers[]` array — the populated slot SUBSCRIBER views in slot order.
     *
     * Each is a zero-copy refcount clone of the stored source view. The FWD resolver ropes
     * these under a fresh PL=1 wrapper into the REPLY (RFC-0004 §D, no byte copy).
     */
    [[nodiscard]] result_t<std::vector<view::view_t>> read_subscribers(
        vertex_handle_t v, std::string_view caller = {}) const;
    /**
     * @brief @ref read_subscribers into a core array (#1781): @p out is replaced by the populated
     *        slot SUBSCRIBER views in slot order, each a refcount clone.
     *
     * The table is drawn from @p out's own source (the caller's choice), so nothing here
     * allocates on the graph's account.
     * @return The number of views written.
     * @retval status_t::PERMISSION_DENIED @p caller lacks READ.
     * @retval status_t::BACKPRESSURE @p out's source refused the table; @p out is left empty.
     */
    [[nodiscard]] result_t<std::size_t> read_subscribers(vertex_handle_t v,
                                                         mem::block_array_t<view::view_t>& out,
                                                         std::string_view caller = {}) const;
    /**
     * @brief Stream history into caller storage, oldest first (Stream role only) — RFC-0028 D11.
     *
     * Fills @p out with the NEWEST `min(out.size(), retained)` ring entries, oldest first, each
     * a @ref value_ref_t share of the retained block: one refcount bump per entry, NO
     * allocation and no byte copy. A span as long as the vertex's `retention_t::N` depth
     * always holds the whole ring. Entries past the returned count are left untouched.
     *
     * @return The number of entries written to @p out.
     * @retval status_t::SCHEMA_NOT_FOUND @p v is not a STREAM.
     * @retval status_t::PERMISSION_DENIED The local caller lacks READ.
     */
    [[nodiscard]] result_t<std::size_t> history(vertex_handle_t v,
                                                std::span<value_ref_t> out) const;

    /**
     * @brief Drain @p v's STREAM entries appended since the last flush, in order — a queue,
     *        not a coalesce (RFC-0008 §E) — and advance the drain cursor.
     *
     * The handle-based mirror of the drain half of §E: the same cursor the internal write and
     * sweep paths advance, reachable by an owner that drives propagation itself. It is the
     * observation seam for §E's "a stream's flush delivers each ring entry appended since the
     * previous flush" — `history` shows what the ring RETAINS, this shows what is OWED.
     *
     * The out-param is deliberate, not a returned vector: `vertex_t::drain_unflushed`'s #477
     * nothrow contract is "on OOM return 0 WITHOUT advancing the cursor, so the entries
     * re-drain on the next covering flush", and that only holds with caller storage the
     * snapshot can nothrow-reserve into. This form inherits that contract verbatim, including
     * the note that entries trimmed out of the keep-last ring before the drain are lost.
     *
     * Draining ADVANCES the cursor, so a later @ref propagate sweep will not re-deliver what
     * this took — the caller now owns delivering them.
     *
     * @param v   The STREAM vertex to drain.
     * @param out Caller storage the drained entries are assigned into (overwritten).
     * @param gap_before Optional out: shed points on this ring since the previous drain — the
     *            in-order `tr::flow::address_shift_gap` signal of RFC-0025 §4.4/§4.5. Non-zero
     *            means entries this consumer would have seen are MISSING immediately before
     *            the returned batch. Written whenever non-null, including on a zero drain, so
     *            polling a quiet ring still surfaces a shed. Silence is the one behaviour the
     *            pressure contract forbids, and this is where it is broken.
     * @return The number of entries drained (0 ⇒ nothing appended since the last flush, or
     *         the snapshot could not be allocated — retry on the next flush).
     * @retval status_t::SCHEMA_NOT_FOUND @p v is not a STREAM — no ring, no cursor, the same
     *         disposition @ref history gives a non-stream role.
     * @retval status_t::PERMISSION_DENIED The local caller lacks READ. A drain hands back the
     *         SAME bytes @ref history serves, so leaving it ungated would be a READ-gate
     *         bypass wearing a different verb's name.
     */
    [[nodiscard]] result_t<std::size_t> drain_unflushed(vertex_handle_t v,
                                                        std::vector<value_ref_t>& out,
                                                        std::uint64_t* gap_before = nullptr);
    /**
     * @brief @ref drain_unflushed into a core array (#1781) — the same contract and the same
     *        refusals. The snapshot is drawn from @p out's own source; a refused reservation
     *        drains nothing (0) and leaves the entries owed to the next covering flush.
     */
    [[nodiscard]] result_t<std::size_t> drain_unflushed(vertex_handle_t v,
                                                        mem::block_array_t<value_ref_t>& out,
                                                        std::uint64_t* gap_before = nullptr);
    /**
     * @brief Advance @p v's STREAM drain cursor to "now" WITHOUT draining (RFC-0008 §E) — an
     *        eager delivery already flushed the ring, so a later sweep must not re-deliver.
     *
     * The handle-based mirror of the flush half of §E, and the exact verb `write_branch`
     * already uses internally after it fans a decomposed slice out eagerly. Takes no cursor
     * argument: the cursor is the per-vertex "appended since flush" count and the only thing
     * a flush can say about it is "nothing is owed".
     *
     * Ungated beyond the role check, unlike @ref drain_unflushed — it discloses no bytes and
     * has no wire surface — the same owner-side shape @ref propagate and @ref set_policy
     * carry.
     *
     * @retval status_t::SCHEMA_NOT_FOUND @p v is not a STREAM.
     */
    [[nodiscard]] result_t<void> mark_flushed(vertex_handle_t v);

    /**
     * @brief FOLDED projection of the `:children` listing (L4 fold, Slice 0) — the SAME
     *        `POINT{ POINT{NAME}… }` that the materialized `read_children` serializes, but
     *        produced as a scatter-gather link chain (an outer POINT header link plus the
     *        member links) instead of one flat buffer, answered as a @ref value_ref_t
     *        (RFC-0028 D11: every value read answers one type).
     *
     * A read-only projection over the materialized tree — the tree stays the source of
     * truth; this walks it and gathers rather than copying the whole listing into a
     * single allocation. `read_children_folded(v)->flatten()` is **byte-identical** to the
     * materialized `read_children` serialize, which `folded_children_test` gates over many
     * graph shapes. The value is valid while the graph (and its insert-only, pointer-stable
     * vertices) outlive it. The synthesized-listing case (ADR-0044) has nothing to gather
     * and crosses as a single-link value. The composed value is one block from the global
     * heap (`value_ref_t::composed`), whose refusal is `BACKPRESSURE`. Each member's NAME bytes are
     * borrowed IN PLACE (zero copy, @ref view::borrow_const) over the pinned child vertex — only
     * the tiny POINT headers are emitted — so the listing is never copied whole.
     */
    [[nodiscard]] result_t<value_ref_t> read_children_folded(vertex_handle_t v) const;

    /**
     * @brief MATERIALIZED `:children` listing — the flat single-link serialize of the same
     *        `POINT{ POINT{NAME}… }` the fold gathers.
     *
     * The production field read serves the FOLDED value; this flat form exists as the
     * independent oracle `folded_children_test` diffs the fold against (byte identity on
     * flatten() over many graph shapes) — without it the differential would be
     * tautological.
     */
    [[nodiscard]] result_t<value_ref_t> read_children_materialized(vertex_handle_t v) const;

    /**
     * @brief COMPOSED BRANCH READ (RFC-0005 §C follow-on): the POINT tree of @p v's
     *        registered subtree, folded as a scatter-gather link chain of views over the
     *        live last-known values (zero flatten, zero byte copies), answered as one
     *        composed @ref value_ref_t (RFC-0028 D11).
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
     * `register_vertex_key` takes raw key bytes and counts nothing, so a vertex can be
     * registered at any depth, and a creation hook may register one at whatever depth the
     * writes it admits reach (RFC-0030 §7). The iterative walk is safe
     * because it is iterative and resource-bounded — which is the real reason, and the only one
     * that survives `kMaxSegments` being lifted.
     *
     * Resolver contract: with a subject resolver installed, `acl_allows` — and therefore
     * the resolver callback — runs O(nodes) times per composed read **under the shared
     * `map_mutex_`**; a resolver MUST NOT re-enter graph mutation APIs (self-deadlock).
     */
    [[nodiscard]] result_t<value_ref_t> read_subtree_folded(vertex_handle_t v,
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
     * @param ctx Caller-owned context, passed back to @p fn on every delivery. Its lifetime
     *            is bounded by this build's reclamation policy (ADR-0080), not by prose: keep
     *            it alive until @ref unsubscribe releases it — under the default
     *            @ref tr::graph::reclaim_local_t that is before `unsubscribe()` returns when
     *            called from outside a delivery, and before the enclosing `write()` returns
     *            when called from inside one. Pass a
     *            @ref tr::graph::subscriber_release_fn_t to
     *            @ref unsubscribe(const subscription_t&, subscriber_release_fn_t) to be TOLD
     *            which; there is no in-flight state to poll. See @ref subscription_t.
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
     * address (lvalues only — a temporary would dangle). Its lifetime bound is the `{fn, ctx}`
     * form's, since it IS the `ctx`: it must stay alive until @ref unsubscribe releases it,
     * which this build's reclamation policy (ADR-0080) pins to a moment the library reaches
     * on its own — see @ref subscription_t.
     * @param policy This subscription's DELIVERY policy (RFC-0022 §3.A); all-zero default.
     * @return A @ref subscription_t handle for @ref unsubscribe (as the `{fn, ctx}` form).
     */
    template <typename F>
        requires std::invocable<F&, const value_t&>
    [[nodiscard]] result_t<subscription_t> subscribe(const path_t& src, F& callback,
                                                     delivery_policy_t policy = {}) {
        return subscribe(
            src, [](void* c, const value_t& v) { (*static_cast<F*>(c))(v); }, &callback, policy);
    }

    /**
     * @brief Remove the in-process subscription @p sub returned by @ref subscribe.
     *
     * The host-SDK-sugar counterpart of the wire `:subscribers[N]` clear (ADR-0049): it
     * deactivates the edge slot and unwinds the RFC-0005 listener bookkeeping (descendants'
     * writes stop bubbling to the producer @p sub names), exactly as the wire path does. The shell
     * stays (index-stable) and a later @ref subscribe reuses it. Idempotent-ish: a
     * default-constructed or already-cleared handle returns `NOT_FOUND`.
     *
     * **On the `ctx`.** Retirement takes effect at once — the next snapshot skips the slot —
     * but a fan-out ALREADY walking a snapshot still names the retired `{fn, ctx}` pair. Under
     * the default @ref tr::graph::reclaim_local_t there is exactly one case where that can be
     * true of THIS call: unsubscribing from inside a delivery. So when this overload is called
     * from outside any delivery — the ordinary case — it returns already quiescent and the
     * caller may free its `ctx` on the return. Called from INSIDE a delivery it cannot tell the
     * caller when the pair died, because it was given no way to: use the two-argument overload
     * below, which is the form that carries a signal. Under
     * @ref tr::graph::reclaim_strict_t re-entrant unsubscribe is forbidden outright, so this
     * overload is always quiescent on return.
     * @note Applies to the callback-form subscriptions; a path→path (`subscribe(src, target)`)
     *       edge is a wire `:subscribers[]` field-write, removed via that wire clear.
     */
    [[nodiscard]] result_t<void> unsubscribe(const subscription_t& sub);

    /**
     * @brief Remove the in-process subscription @p sub and be TOLD when its `ctx` is dead —
     *        ADR-0080's event-driven half.
     *
     * Identical to the one-argument overload in what it retires; it adds the one thing that
     * overload structurally cannot provide, a **signal**. @p release is invoked exactly once
     * with the subscription's `callback_ctx`, on THIS thread, outside every graph lock, at the
     * grace point the bound @ref tr::graph::default_config_t::reclaim_policy_t names:
     *
     * | bound policy | called from OUTSIDE a delivery | called from INSIDE one |
     * | --- | --- | --- |
     * | @ref tr::graph::reclaim_strict_t | inline, before this call returns | forbidden |
     * | @ref tr::graph::reclaim_local_t | inline, before this call returns | before the enclosing
     *   `write()` / `propagate()` returns |
     * | @ref tr::graph::reclaim_qsbr_t | inline when NO participant is mid-dispatch | once every
     *   participant has passed a quiescent state — possibly on another thread |
     *
     * So the caller frees its context from @p release and never asks a question about
     * in-flight state — the library owns that tracking. A hook is run ONLY for a call that
     * actually retired an edge: a `NOT_FOUND` return (a default-constructed handle, an
     * already-cleared slot) owes no signal and runs nothing.
     *
     * @param sub     The handle @ref subscribe returned.
     * @param release The release hook; `nullptr` degrades this to the one-argument overload.
     *                It must not itself unsubscribe the same handle, and it runs on whichever
     *                thread reached the grace point.
     * @return `{}` on success; `NOT_FOUND` when @p sub names no active edge — and then
     *         @p release is *not* called.
     * @note A deferred hook needs one of the
     *       @ref tr::graph::default_config_t::kDeferredReleaseSlots parking slots — this
     *       thread's, or under `reclaim_qsbr_t` the shared table's. If every one is taken the
     *       pair is DROPPED and the hook never runs — a deliberate leak in preference to a
     *       use-after-free — and @ref deferred_release_drops counts it.
     */
    [[nodiscard]] result_t<void> unsubscribe(const subscription_t& sub,
                                             subscriber_release_fn_t release);

    /**
     * @brief Suspend or resume the in-process subscription @p sub without removing it (#1533).
     *
     * The edge keeps its slot index, its callback pair, its binding and its admission
     * decision; only its suspended flag moves, and no SUBSCRIBE gate runs again. A suspended
     * edge leaves the RFC-0005 counts (@ref own_subs, @ref has_subscribers, the ancestors'
     * bubbling count), so a vertex whose every edge is suspended skips the fan-out exactly as
     * one with none does; while other edges deliver, each suspended one costs a write one
     * skipped entry of the published array. A resume replays nothing: the edge delivers from
     * the next propagated value on, and durability stays a join-time property.
     *
     * **What a toggle costs.** A flip of the edge's published entry under the vertex's stripe
     * lock, plus the counter walk over the vertex's descendants that a subscribe also pays:
     * no republish, and nothing drawn from any source, so a switch (one suspend plus one
     * resume) allocates nothing and sends nothing. A suspend cannot fail. The one exception
     * is a resume after an edge republish on this vertex was refused for want of memory: the
     * array may then name the slot's previous occupant, so the resume rebuilds it first, the
     * one draw a toggle makes, from the vertex's own edge source. If that draw is refused too,
     * it answers BACKPRESSURE with nothing changed, and a retry succeeds once the source has
     * room again.
     *
     * **The guarantee, exactly.** This is not a grace point. A fan-out that took its snapshot
     * before the flip — concurrently on another thread, or re-entrantly up this one's stack —
     * still delivers to the edge once AFTER this returns; every snapshot taken after the flip
     * skips it. That is @ref unsubscribe's guarantee without the grace point, and a context is
     * freed only through @ref unsubscribe.
     *
     * @warning A @ref subscription_t carries no generation (#1932): a stale handle whose slot
     *          was cleared and then reused by a later subscribe names THAT edge, and suspends
     *          or reads it (a cleared shell is reused by a later @ref subscribe, as
     *          @ref unsubscribe notes).
     * @note The callback-form subscriptions only, as for @ref unsubscribe.
     * @retval NOT_FOUND    No live edge @p sub names.
     * @retval BACKPRESSURE A resume that had to rebuild a stale edge array (see above) and was
     *                      refused the draw; the edge stays suspended and a retry may succeed.
     */
    [[nodiscard]] result_t<void> set_suspended(const subscription_t& sub, bool suspended);

    /**
     * @brief Is the in-process subscription @p sub suspended (#1533)?
     * @warning The slot-reuse caveat of @ref set_suspended applies.
     * @retval NOT_FOUND No live edge @p sub names.
     */
    [[nodiscard]] result_t<bool> is_suspended(const subscription_t& sub) const;

    /**
     * @brief How many retired `{ctx, release}` pairs this PROCESS has dropped for want of a
     *        parking slot — each one a release hook that will never run (ADR-0080).
     *
     * The observability half of the bounded park: a leak is the safe answer to an exhausted
     * bound, but a silent one is not. A non-zero reading means
     * @ref tr::graph::default_config_t::kDeferredReleaseSlots is undersized for how many
     * subscriptions this node retires from inside a single delivery stack — raise it in the
     * override fragment. It is process-wide (summed across threads) and monotonic, and it
     * stays 0 forever on a node that never unsubscribes re-entrantly, which is most of them.
     *
     * Under @ref tr::graph::reclaim_qsbr_t it counts the SHARED retired table's drops and means
     * something sharper: that domain republishes and rescans before it gives up, so a non-zero
     * reading says a participant thread genuinely never reached a quiescent state while the
     * table filled. That is an embedder defect — a dispatching thread that never returns to its
     * event loop, or more concurrent dispatchers than
     * @ref tr::graph::default_config_t::kQsbrParticipants — and this counter is how it surfaces.
     */
    [[nodiscard]] static std::uint64_t deferred_release_drops() noexcept;

    /**
     * @brief Declare that this thread holds no in-flight delivery — the OPT-IN quiescent point
     *        of a cross-thread grace period (ADR-0080, #1376).
     *
     * **No policy's guarantee depends on an embedder calling this**, and that is deliberate: it
     * would otherwise contradict ADR-0080 §Decision 4 and the reference article's "there is no
     * verb the embedder must remember to call". Every dispatching thread announces and drains
     * AUTOMATICALLY at its outermost dispatch exit, which already covers ADR-0080 §Decision 3's
     * named case — an RX thread that has finished a message and returned to its event loop.
     *
     * It exists for the thread the automatic path cannot reach: one that mutates LKV slots
     * (displacing nodes onto its own private retired list) **without ever dispatching**, and
     * one that wants a stated teardown precondition before an injected `%std::pmr` resource
     * dies (ADR-0039 §Erratum 8's "domain quiescence point", which #897 asks be nameable). Call
     * it at the top of an event loop, or once before joining such a thread.
     *
     * Under the two per-thread policies it is an empty inline function — it compiles to
     * literally nothing, and `graph.cpp` is not even aware of it. It must NOT be confused with
     * @ref collect, which is the ADR-0072 value-seam park and answers an unrelated question;
     * this verb is not a poll of in-flight state and returns nothing.
     */
    static void thread_quiescent() noexcept { pass_quiescent_state<reclaim_policy_t>(); }

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
     * @brief Replace the graph's five wiring seams WHOLE (RFC-0028 §4.12, D12) — the one
     *        verb that replaced the five `configure_*` verbs.
     *
     * The constructor takes the application's hooks; this verb exists for the party that
     * can only be built AFTER the graph — `tr::net::fwd_router_t`, whose constructor takes
     * the graph and installs its three transport-plane seams (`remote_delivery`,
     * `wire_target`, `stats_sampler`) by reading @ref hooks, filling its slots and handing
     * the whole struct back, so the application's two stay as they were.
     *
     * CONFIGURATION, not a runtime knob (#1049): from one thread, before frames flow. Each
     * slot is published through its own `tr::sink_slot_t`, so a dispatch racing the install
     * sees a whole new pair, a whole old one, or none — never a new `fn` beside a stale
     * `ctx`; what it does NOT do is stop a dispatch already in flight, so every `ctx` must
     * outlive every dispatch that can still reach its seam.
     */
    void set_hooks(const graph_hooks_t& hooks) noexcept;

    /** @brief The five wiring seams as currently installed — the read half of
     *         @ref set_hooks (one coherent snapshot per slot). */
    [[nodiscard]] graph_hooks_t hooks() const noexcept;

    /**
     * @brief Ask the installed sampler for one net-plane seam — the read side of
     *        @ref graph_hooks_t::stats_sampler.
     *
     * Public because the census encoder is a free function over `graph_t`'s public accessors
     * (it needs no friendship and mints no other symbol), and useful on its own to an
     * in-process supervisor that wants one seam's block without going through the wire door.
     *
     * @param seam_class The seam CLASS (`router`, `labels`, `link`).
     * @param seam_name  The seam NAME within it.
     * @param out        Where to write the block, or `nullptr` to probe recognition only.
     * @return `false` when no sampler is installed or the spelling names no seam.
     */
    [[nodiscard]] bool sample_stats(std::string_view seam_class, std::string_view seam_name,
                                    stats_block_t* out) const noexcept;

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
     *
     * @p caller is the SUBJECT this admission is gated under and the fan-in context every
     * later delivery through the edge re-gates under — *who subscribed*, as against @p link's
     * *where to deliver* (ADR-0082 §Decision 1; the two were one string until #375 Part 2).
     * EMPTY means "the same as @p link", which is every pre-split caller and is byte for byte
     * what this door did before: a FLAT listener's peers all subscribed as their shared link
     * name. A terminus that derived a per-writer subject from the frame's `peer_handle_t`
     * passes it here, and then the SUBSCRIBE gate, the stored `subscriber_remote_t::caller`
     * and each delivery's WRITE re-gate all name the writer rather than the wire it used.
     * The delivery link is unaffected, which is the point of splitting them: the edge still
     * routes back over @p link (or, for a mount-routed target, over the mount).
     *
     * @p link_token is the CARRIED interned identity of @p link (#1266 / #1417) — what the
     * transport plane got back from @ref intern_link at link-up and cached in its own
     * per-link receive context. It saves the index the name hash and find — **42–56 % of the
     * whole index operation net of its control**, measured at all ten cells of #1416's own
     * harness. A token that cost NOTHING to obtain would be worth 72–82 %, so roughly two
     * thirds of that ceiling survives paying for the carry. It is an OPTIMISATION and only
     * that:
     * the default is "no token", which is byte-identical to every pre-carry caller, and a
     * token that does not name a slot holding the key the index is about to use is ignored
     * rather than trusted. That is deliberate — the key is not always @p link (a mount-routed
     * target rebinds it to the mount's, and a `field_write` admission has only its `caller`,
     * #943), and a token silently indexing under the wrong link is exactly the leaked
     * subscriber edge #1071 exists to prevent.
     */
    [[nodiscard]] result_t<void> subscribe_wire(vertex_handle_t v, view::view_t source_view,
                                                view::view_t return_route, std::string_view link,
                                                view::view_t reverse_route = {},
                                                std::string_view caller = {},
                                                link_id_t link_token = {});

    /**
     * @brief Read by path — resolve the path key once (guarded map lookup), then the hot path.
     *
     * A read whose path has a field tail (e.g. `:settings.app.kp`, `:subscribers[]`,
     * `:schema`) is routed to the field surface.
     */
    [[nodiscard]] result_t<value_ref_t> read(const path_t& path) const;
    /** @brief Write by path — resolve the key once, then @ref write(vertex_handle_t, view::rope_t,
     * std::string_view, const net::link_kind_t*). */
    [[nodiscard]] result_t<void> write(const path_t& path, view::rope_t value);
    /** @brief Await by path — resolve the key once, then @ref await(vertex_handle_t,
     * std::chrono::nanoseconds, std::string_view). */
    [[nodiscard]] result_t<value_ref_t> await(const path_t& path, std::chrono::nanoseconds timeout);

    /** @brief Resolve a canonical PATH-payload @p key to its vertex handle (`nullopt` if
     *         unknown). */
    [[nodiscard]] std::optional<vertex_handle_t> find(std::span<const std::byte> key) const;

    /**
     * @brief @p v's copy-or-share threshold in bytes (RFC-0028 §5.3): what it declared with
     *        @ref vertex_policy_t::share_threshold_bytes, else @ref
     * tr::graph::config_t::kShareThresholdBytes.
     *
     * The read accessor the opaque handle does not expose directly: the WRITE resolver
     * (`%op_resolve_walk.hpp`) queries it here instead of dereferencing the vertex. One
     * inline load, and nothing is inherited (RFC-0022 §3.F).
     */
    [[nodiscard]] std::size_t share_threshold_bytes(vertex_handle_t v) const noexcept;

    /**
     * @brief Resolve the target of a fieldless DATA write at @p key, creating a missing level
     *        only where a parent's creation hook opts it in (RFC-0030 §7).
     *
     * A hit returns the vertex. A miss answers `NOT_FOUND` and creates nothing, whatever the
     * write's origin, unless the build allows creation hooks (`config_t::kCreationHooks`, off
     * by default) and the parent of the first missing level carries one. Then the parent's
     * `CREATE` right is evaluated for @p caller (`PERMISSION_DENIED`: the hook does not run and
     * @p payload is not asked for), and the hook decides. A level the hook created is the parent of
     * the next, so a deeper miss is decided by that new vertex's own hook: `mkdir -p` is
     * expressible only where every level opted in.
     *
     * Only a fieldless data write may create (§7.1). Every other request (a read, a `:field`
     * write, any op without a payload) resolves with @ref find instead, so a miss is
     * `NOT_FOUND` without consulting a hook or the `CREATE` gate.
     *
     * @param payload Produces the written payload the hook is shown, and is called only when a
     *        hook is about to decide, after the `CREATE` gate admitted @p caller. It returns an
     *        empty rope when the payload could not be held (`BACKPRESSURE`). It may be called
     *        more than once for one write.
     * @retval INVALID_PATH @p key is not a well-formed canonical PATH payload (checked before a
     *         hook creates anything).
     */
    [[nodiscard]] result_t<vertex_handle_t> find_or_create(
        std::span<const std::byte> key, std::string_view caller,
        function_ref_t<const view::rope_t&()> payload);

    /**
     * @brief Install @p hook as @p parent's creation hook (RFC-0030 §7.2), replacing any
     *        previous one. An empty @p hook removes it.
     *
     * Local host API only: no wire operation reaches here. The hook is a declaration of the
     * parent's current owner, so retiring @p parent drops it. Its `ctx` must outlive the
     * installation.
     *
     * @retval SCHEMA_NOT_FOUND This build does not allow creation hooks
     *         (`config_t::kCreationHooks` is `false`, the default): no vertex has the slot, so
     *         the request is refused rather than dropped.
     * @retval NOT_FOUND @p parent is retired or was never registered.
     * @retval BACKPRESSURE The graph's table source refused the declaration; nothing changed.
     */
    [[nodiscard]] result_t<void> set_creation_hook(vertex_handle_t parent, creation_hook_t hook);

    /**
     * @brief Drop @p vh out of its parent's `:children[]` listing, keeping it registered and
     *        addressable — the enumeration-hide seam
     *        [RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
     *        §3 requires (stage S4 of
     * [#492](https://github.com/avatarsd-llc/libtracer/issues/492)).
     *
     * The one caller today is @ref tr::net::transport_vertex_t, on the creator endpoint
     * `<net_root>/<module>/conn`: §3 reserves that name and says it is **hidden** from
     * `<net_root>/<module>:children[]`, which "returns the member connection vertices". The
     * endpoint is not one of them — it is the write-only control that CREATES them — so a
     * peer walking the listing as a topology of links descends into a vertex with no peer
     * behind it. Discovery of the endpoint is by §6's creatability probe (`read
     * <module>/conn:schema`) instead, which is why hiding must not cost addressability.
     *
     * Scope, deliberately: this changes `:children[]` (both the materialized and the folded
     * door) and nothing else. @ref find still resolves the vertex, reads and writes still
     * reach it, the RFC-0016 composed branch read still descends into it, and the owner-side
     * @ref for_each_vertex census still visits it. RFC-0014 §3's clause is about the member
     * listing; widening it to the other surfaces is not this seam's call to make.
     *
     * One-way. The bit belongs to the current occupant of the key, so a @ref retire clears it
     * along with the rest of that occupant's identity; a fresh registration is listed again
     * unless it hides itself. There is no `unhide`.
     *
     * @param vh The vertex to hide. Must be registered.
     * @retval status_t::NOT_FOUND @p vh is null or is an unregistered placeholder — hiding a
     *         vertex that is not a member yet would silently apply to whatever registers
     *         there later.
     */
    [[nodiscard]] result_t<void> hide_from_enumeration(vertex_handle_t vh);

    /**
     * @brief How many writes performed the ancestor (bubbling) walk — instrumentation.
     *
     * The near-free-when-idle observable (RFC-0005): stays 0 while no subscriber
     * exists above any written vertex, so tests and benches can assert a write
     * never walks ancestors unless someone is listening. Relaxed monotonic counter.
     *
     * Compiled in only when @ref default_config_t::kInstrumentCounters is bound `true`; on a
     * lean build (the default) the counter does not exist and this answers `0` (#1664).
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
     *
     * Compiled in only when @ref default_config_t::kInstrumentCounters is bound `true`; on a
     * lean build (the default) the counter does not exist and this answers `0` (#1664).
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
     * sheds every edge past the inline prefix, and an `assign` whose pending mark cannot be
     * allocated sheds the vertex's WHOLE subscriber set — each shed delivery is one
     * increment, so `1` never stands in for `N`. A handler write's notify clone used to be
     * the widest of these; #1505 deleted the clone rather than the tally, so that shed is
     * now impossible rather than counted, and the width rule is unchanged by its removal.
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
    /**
     * @brief The `:`-field surface (#1711): one constant row per colon field, defined in
     *        `core/src/graph_fields.cpp` beside the two doors (`read_field_composed`,
     *        `field_write`) that dispatch on it. A nested type rather than a friend, so no
     *        class outside `graph_t` can claim its access.
     */
    struct field_surface_t;
    /** @brief Apply a legal @ref vertex_policy_t to @p vx all-or-nothing, skipping every
     *         member that holds. @p role is the role @p vx has, or is about to be filled with.
     *         The delivery mode is applied only when @p mode_key (@p vx's canonical key) is
     *         not empty, which the caller passes when the mode moves.
     *  @retval false The table source refused a block (#1778, #1883); no member changed. */
    [[nodiscard]] bool apply_policy(vertex_t* vx, role_t role, vertex_policy_t&& policy,
                                    std::span<const std::byte> mode_key);

    /** @brief Set @p v's propagation policy and maintain the sweep's UNCONDITIONAL membership
     *         under the sweep lock (RFC-0008 §C) — `apply_policy`'s commit point. A
     *         registration calls it under the map lock; the sweep lock never takes the map
     *         lock under it (ADR-0057). @p key is @p v's canonical key.
     *  @retval false The table source refused the set entry (#1778); nothing changed. */
    [[nodiscard]] bool apply_delivery_mode(vertex_t* v, delivery_mode_t mode,
                                           std::span<const std::byte> key);

    // Internal (raw `vertex_t*`) forms of the public handle-returning resolvers: the graph's
    // own machinery threads raw pointers (ADR-0056 — internal methods keep `vertex_t*`), and
    // the public @ref find / @ref find_or_create wrap these once at the boundary.
    [[nodiscard]] vertex_t* find_ptr(std::span<const std::byte> key) const;
    [[nodiscard]] result_t<vertex_t*> find_or_create_ptr(
        std::span<const std::byte> key, std::string_view caller,
        function_ref_t<const view::rope_t&()> payload);
    // The whole body of @ref register_vertex_key, over BORROWED key bytes. The descent never
    // retains the key, so the public owning-vector overload is a convenience wrapper and the
    // graph's own callers (path registration) pass a span rather than paying a
    // heap copy just to spell the call (#1139/#873).
    // @p policy is refused before the descent when it is illegal for @p role, and otherwise
    // applied to the node before it is filled, its delivery mode included, so a refused one
    // leaves it a placeholder (#1778, #1920). The default policy touches nothing on a
    // placeholder, which carries none.
    [[nodiscard]] result_t<vertex_handle_t> register_vertex_key_span(
        std::span<const std::byte> key, role_t role, const handlers_t& handlers,
        std::span<const payload_right_t> rights = {},
        std::span<const std::byte> schema_catalog = {}, vertex_policy_t policy = {});
    // Update the vertex value (LKV/history/handler), then fan out to subscribers.
    // `caller` is the ACL caller context gating the WRITE right (the API caller's
    // for a direct write; a delivered subscription's stored context terminates at
    // its target instead — see dispatch_edge_target, ADR-0051).
    // `link` is the arrival link's catalog identity (#1650), null for a write no link carried;
    // it travels beside `caller` to the one place it is read — the `write_ctx_t` the admission
    // filter and the handler receive. REQUIRED on every internal leg, for `caller`'s reason:
    // a new path must say which it is, not default into "no link".
    [[nodiscard]] result_t<void> write_impl(vertex_t* v, view::rope_t value,
                                            std::string_view caller, const net::link_kind_t* link);
    // write_impl's `retention_t::NONE` arm (RFC-0028 §5.4): admit, bump the sequence, and
    // deliver from the stack — no block, nothing retained.
    [[nodiscard]] result_t<void> relay_write(vertex_t* v, view::rope_t value,
                                             std::string_view caller, const net::link_kind_t* link);
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
    // The Handler leg MOVES the links out of `value` into stack storage (RFC-0028 D10), so on
    // every arm the caller's rope is consumed by the call — never read it afterwards.
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
    // the owner's own. It is also `write_ctx_t::subject` for the retaining roles' ADMISSION
    // filter (`handlers_t::on_admit`), which runs here — above the storing tail, so a refusal
    // never becomes state and a normalisation is the only value any reader can reach.
    // `take` is the write path's fused drain (#1713), forwarded to publish_value: non-null only
    // from write_impl's STREAM arm, which delivers what the admitting section took.
    [[nodiscard]] result_t<value_ref_t> store_value(vertex_t* v, view::rope_t&& value,
                                                    vertex_t::store_drops_t& drops,
                                                    std::string_view caller,
                                                    const net::link_kind_t* link,
                                                    vertex_t::ring_take_t* take = nullptr);
    /**
     * @brief The ADOPTING store (RFC-0028 D2, slice 4): publish a value some other vertex
     *        already published by taking one more reference on its block — no clone, no
     *        allocation.
     *
     * The target leg of a subscription delivery lands here, so K target subscribers cost K
     * refcount bumps rather than K blocks. Same gates and same tail as the rope overload:
     * the admission filter runs over a `rope_t` of the same links (only a normalisation mints
     * a block), the ring admission charges the receiving vertex's own source, and `drops` is
     * zeroed on entry. A HANDLER target is handed @p value itself by reference (RFC-0028
     * D10) — no clone, no block. Caller-owned storage (`value.source() == nullptr`, a
     * `value_storage_t` a branch write delivers without storing — a kept reference would
     * outlive its frame) takes the rope overload instead, through one nothrow clone of the
     * links.
     * @return The published reference (the same block as @p value on the adopting arm), the
     *         empty "consumed" sentinel on a HANDLER, or the refusal by value.
     */
    [[nodiscard]] result_t<value_ref_t> store_value(vertex_t* v, const value_t& value,
                                                    vertex_t::store_drops_t& drops,
                                                    std::string_view caller,
                                                    const net::link_kind_t* link);
    /**
     * @brief Run @p v's admission filter over @p value under @p caller — the one call both
     *        `store_value` overloads make, so the filter has a single spelling.
     * @return Disengaged to admit unchanged, engaged to store the normalised rope instead, or
     *         the filter's refusal. A vertex whose filter is mid-retire admits.
     */
    [[nodiscard]] admission_t admit(vertex_t* v, const value_t& value, std::string_view caller,
                                    const net::link_kind_t* link) const;
    /**
     * @brief The HANDLER leg of both `store_value` overloads: hand @p value to @p v's
     *        `on_write` by reference (RFC-0028 D10) and store nothing.
     * @return The empty "consumed" sentinel, `NOT_FOUND` when no `on_write` is installed, or
     *         the handler's own refusal.
     */
    [[nodiscard]] result_t<value_ref_t> handler_write(vertex_t* v, const value_t& value,
                                                      std::string_view caller,
                                                      const net::link_kind_t* link);
    /**
     * @brief The rope arm's HANDLER leg: move a local write's links into stack storage (or,
     *        past `kUnstoredInline` links, one block) and run %handler_write over it.
     */
    [[nodiscard]] result_t<value_ref_t> handler_write_rope(vertex_t* v, view::rope_t&& value,
                                                           std::string_view caller,
                                                           const net::link_kind_t* link);
    /**
     * @brief `write_impl`'s HANDLER arm: build ONE value from @p value (moved onto this frame,
     *        or one block past `kUnstoredInline` links), hand it to `on_write`, then deliver the
     *        same value to the vertex's own subscribers (#1505: no clone).
     */
    [[nodiscard]] result_t<void> handler_write_deliver(vertex_t* v, view::rope_t&& value,
                                                       std::string_view caller,
                                                       const net::link_kind_t* link);
    /**
     * @brief The storing tail every non-HANDLER store shares: publish @p sp to @p v's slot,
     *        then admit it into @p v's ring when @p v is a STREAM.
     * @param sp The value to publish; EMPTY means the block could not be minted, and is
     *           answered as BACKPRESSURE like a declined slot.
     * @param take Optional (#1713): the stack-first buffer the ring admission takes the
     *           unflushed window into, in the SAME stripe section — the write path's one lock.
     */
    [[nodiscard]] result_t<value_ref_t> publish_value(vertex_t* v, value_ref_t sp,
                                                      vertex_t::store_drops_t& drops,
                                                      vertex_t::ring_take_t* take = nullptr);
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
    [[nodiscard]] result_t<void> write_branch(vertex_t* v, const view::rope_t& value,
                                              std::string_view caller, const net::link_kind_t* link,
                                              bool notify);
    void fan_out(vertex_t* v, const value_t& value);
    // The same fan-out over a SLICE no vertex stored (a branch write's per-site cut): the view
    // is wrapped in stack storage for the duration of the dispatch.
    void fan_out_slice(vertex_t* v, const view::view_t& slice);
    // The ONE dispatch of a subscription edge's three legs (in-process callback, local
    // target re-dispatch, remote sink) — shared by fan_out and the admission durability
    // latch (ADR-0049), always called OUTSIDE the vertex lock. The target/remote legs
    // are split out so the per-edge body stays small enough to inline into the fan-out
    // loop (the wide-fan-out hot loop; the callback leg is the in-process hot case).
    void dispatch_edge(const edge_view_t& e, const value_t& value);
    // A SUBSCRIBER delivery TERMINATES at its target (ADR-0051 / RFC-0007): apply the
    // target-local effects of a write — store (LKV/history per role) + await wake + the
    // target's own handler reaction — gated by the TARGET's WRITE :acl, and NEVER
    // re-dispatch to the target's own :subscribers[]. Propagation past a target is the
    // target's own logic; a dispatch cycle is impossible by construction (no depth cap).
    void dispatch_edge_target(const edge_view_t& e, const value_t& value);
    void dispatch_edge_remote(const edge_view_t& e, const value_t& value);
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
    void bubble_up(vertex_t* v, const value_t& value);
    // Deliver an UNSTORED rope (a HANDLER write's value, a branch write's whole tree) through
    // `fn` as the `value_t` the receivers read: stack storage while the chain fits
    // kUnstoredInline links (the #1505 no-clone property, kept), else one block from the
    // graph's source whose refusal sheds the delivery — counted OUT_OF_MEMORY at `width`.
    static constexpr std::size_t kUnstoredInline = 8;
    void deliver_unstored(vertex_t* v, const view::rope_t& value,
                          void (graph_t::*fn)(vertex_t*, const value_t&), std::size_t width);
    // Deliver `value` as `v`'s value to v's full observer set: v's own edges (fan_out)
    // + every ancestor subtree subscriber (bubble_up, gated on listeners_above_). The
    // per-vertex delivery unit both `write` (eager) and `propagate` (sweep) build on.
    void deliver_vertex(vertex_t* v, const value_t& value);
    // Deliver v's CURRENT stored value (propagate reads the LKV — no value argument).
    // STORED_VALUE: the last-known-value once; STREAM: drains the ring entries appended
    // since the last flush, in order (RFC-0008 §E — a queue, not a coalesce); HANDLER /
    // never-assigned (null LKV): nothing.
    void deliver_current(vertex_t* v);
    /**
     * @brief A flat list of vertex keys drawn from one source (#1778): every key's bytes in
     *        one array plus where each ends, so collecting N keys is two growing arrays, not
     *        N owned buffers.
     */
    class key_list_t {
       public:
        /** @brief An empty list drawing from @p src. */
        explicit key_list_t(mem::block_source_t& src) noexcept : bytes_(src), ends_(src) {}
        /** @brief Append @p k. @retval false The source refused; the list is unchanged. */
        [[nodiscard]] bool push(std::span<const std::byte> k) noexcept {
            // Both arrays grow by doubling, so N pushes copy O(N) bytes (an exact
            // `reserve(size() + 1)` regrew on every push). A refused end entry takes the
            // appended bytes back off, which shrinks and so cannot fail.
            const std::size_t was = bytes_.size();
            if (!bytes_.append(k.data(), k.size())) return false;
            if (ends_.push_back(bytes_.size())) return true;
            (void)bytes_.resize_for_overwrite(was);
            return false;
        }
        /** @brief Keys held. */
        [[nodiscard]] std::size_t size() const noexcept { return ends_.size(); }
        /** @brief Key @p i (`i < size()`). */
        [[nodiscard]] std::span<const std::byte> operator[](std::size_t i) const noexcept {
            const std::size_t b = i == 0 ? 0 : ends_[i - 1];
            return {bytes_.data() + b, ends_[i] - b};
        }

       private:
        mem::bytes_t bytes_;                   /**< @brief Every key's bytes, back to back. */
        mem::block_array_t<std::size_t> ends_; /**< @brief Where each key ends. */
    };
    /** @brief A byte-ordered set of vertex keys (the sweep sets). The value is the key's vertex
     *         — null only for an UNCONDITIONAL enrollment whose registration has not created
     *         it yet — so `retire` can tell a retiree's entry from a newcomer's (#1884). */
    using key_set_t = mem::chunked_map_t<mem::bytes_t, vertex_t*, mem::bytes_less_t>;
    // The propagate(v) sweep body: delivers v then its qualifying descendants
    // (RFC-0008 §B/§C). Loop-free by construction (each delivery terminates at its
    // target — ADR-0051), so no recursion depth to thread.
    void propagate_impl(vertex_t* v);
    // The FOLD emission of the same sweep (RFC-0025 §4.1.2 clause 5): frame the selection as
    // ONE RFC-0005 §B branch-write POINT tree and deliver it, instead of one per-vertex
    // delivery each. Validates the whole selection BEFORE draining a single pending mark, so a
    // refusal is a no-op the caller can retry as PER_VERTEX. Never touches the receive path.
    result_t<void> propagate_folded_impl(vertex_t* v);
    // The selection half of a FOLD sweep: render v's key into `lo` and collect every
    // qualifying strict descendant's key into `out` — the same two sets, the same subtree
    // predicate and the same delivery_mode gating propagate_impl uses, so the two emissions
    // cannot select differently. PEEKS ONLY: nothing is drained, because the fold must be
    // able to refuse an unencodable selection without having consumed it. The marks are
    // retired afterwards by `clear_pending`, exactly as the eager branch write retires its
    // landing sites. False on an OOM key render — the sweep defers, as it does today.
    [[nodiscard]] bool select_sweep(vertex_t* v, mem::bytes_t& lo, key_list_t& out);
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
    void clear_pending(vertex_t* v, const value_t* delivered);
    // Subscribe/unsubscribe bookkeeping (RFC-0005): bump v's own active-slot count
    // and every descendant's listeners_above_, under the map lock (shared — the
    // counters are atomics; the lock only excludes concurrent vertex creation so
    // a newborn's creation-time sum and this walk never double-count).
    void note_subscriber_added(vertex_t* v);
    void note_subscriber_removed(vertex_t* v);
    // Clear subscriber slot `slot` of `v` and, iff it was active, unwind the bookkeeping above
    // and report the removal to the subscription observer under `caller` (a no-op for the
    // empty, local caller): the one clear `unsubscribe` and the wire `:subscribers[N]` clear
    // both run (#1711). False iff the slot was not active. `retired_ctx` as for
    // vertex_t::clear_edge.
    [[nodiscard]] bool clear_subscriber_slot(vertex_t* v, std::size_t slot, std::string_view caller,
                                             void** retired_ctx = nullptr);
    // The single SUBSCRIBER admission step (ADR-0049): SUBSCRIBE gate under `caller` →
    // slot append → transient-local durability latch (delivered outside the lock, per
    // the edge's kind) → RFC-0005 bookkeeping. Every door — the two subscribe() sugars,
    // the local `:subscribers[]` field-write, and the wire subscribe_wire — ends here,
    // so gate and latch semantics cannot diverge per entry point.
    // `link_token` is the carried interned identity (#1417), forwarded verbatim to
    // `link_index_t::index_vertex` — which ignores it unless it names a live slot spelling the key
    // this admission actually indexes under. Defaulted, so the three local doors that carry
    // none are unchanged.
    [[nodiscard]] result_t<subscription_t> admit_subscriber(
        vertex_t* v, subscriber_t s, std::string_view caller,
        std::optional<std::size_t> slot = std::nullopt, link_id_t link_token = {});
    // Fire the external-subscription observer for ONE slot mutation
    // (graph_hooks_t::subscription_observer). Returns immediately when no observer is installed or
    // `caller` is EMPTY — the latter is the whole external/local discrimination, in one place.
    // `sub_tlv` is the slot's stored SUBSCRIBER TLV (empty for a callback-only slot, which no
    // external door can create); the event's target key is decoded from its PATH child. Called with
    // NO graph lock held, after the mutation has landed — see sub_observer_fn_t's re-entrancy
    // warning.
    void notify_subscription(sub_event_t::kind_t kind, const vertex_t* v, std::string_view caller,
                             const view::view_t& sub_tlv, std::size_t slot) const;
    // Report @p n routed edges over `link` established or torn down through the
    // `link_hold` seam (#1816). An empty `link` — a local edge, a field-write edge, or no
    // edge at all — reports nothing. Called with NO graph lock held.
    // Out of line and cold on purpose: it runs on subscribe and teardown only, and letting it
    // inline at its six call sites re-partitioned this TU's budget onto the fan-out copy
    // loop (`vertex_t::copy_published` +277 B on the symbol ratchet).
    [[gnu::noinline, gnu::cold]] void hold_link(std::string_view link, bool held,
                                                std::size_t n = 1) const;
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
    // ":subscribers[]" / "[N]", ":children[]". `ctx.subject` is the ACL caller context every
    // row gates on; the whole context reaches the app-field admission filter (#1832).
    [[nodiscard]] result_t<void> field_write(vertex_t* v, const field_path_t& field,
                                             const view::view_t& value, const write_ctx_t& ctx);
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
    /** @brief The @ref subject_lookup_fn_t that adapts a resolver installed in the returning
     *         form (@ref graph_hooks_t::subject_resolver): @p ctx is the graph, and the token
     *         the resolver returns is copied into @p out. */
    static std::expected<void, wire::err_t> lookup_via_resolver(void* ctx, std::string_view caller,
                                                                mem::bytes_t& out);
    // Subtree-precise ADR-0050 cache invalidation: mark `v` and every descendant's
    // cached effective-ACE merge stale (release stores) after a :acl write on `v`,
    // via the ADR-0057 child links — wiring-frequency. Call with map_mutex_ held
    // (shared suffices; the walk only excludes concurrent vertex creation, and a
    // vertex created after the mark starts dirty anyway).
    static void mark_subtree_acl_dirty(vertex_t* v);
    // ":children[]" append: instantiate a child from a SPEC via the type catalog (#82,
    // ADR-0017). Composes the child key (parent key + the SPEC `name` NAME), dispatches
    // on the SPEC `type`. Unknown type => SCHEMA_NOT_FOUND; duplicate name => PATH_IN_USE.
    [[nodiscard]] result_t<void> create_child(vertex_t* parent, const view::view_t& spec_value);
    // The HANDLER-role arm of the read contract (compose from `on_read`, else NOT_FOUND),
    // shared by BOTH read doors — `read` and, per RFC-0008 Amendment 2, `await` — so the
    // readiness form cannot drift from the form it names. ALREADY-GATED: each door checks
    // acl_right_t::READ up front and this arm does not re-check. Out of line so the
    // retaining arm of either door keeps its `read_stored()` fast path unencumbered.
    [[nodiscard]] result_t<value_ref_t> read_handler_gated(vertex_t* v) const;
    // A COMPOSED read's value (a folded listing, a folded subtree, a field TLV) given a
    // published value's shape: one block from the graph's value source (#2052), whose refusal
    // is BACKPRESSURE by value (#477), never a throw.
    [[nodiscard]] result_t<value_ref_t> composed_or_backpressure(view::rope_t&& r) const noexcept;
    // The field read's composing arms — every `:field` shape but the empty one — each
    // answering the one read type (RFC-0028 D11); the public field `read` forwards to it.
    [[nodiscard]] result_t<value_ref_t> read_field_composed(vertex_handle_t v,
                                                            const field_path_t& field,
                                                            std::string_view caller) const;
    // ":schema" read => a POINT descriptor (name + settings).
    [[nodiscard]] result_t<view::view_t> read_schema(vertex_t* v) const;
    // ":identity" read => the node-scoped SETTINGS{kind,key} record (RFC-0011 §B), or
    // SCHEMA_NOT_FOUND when no keypair is installed. Takes no vertex: the identity is
    // the NODE's, and every vertex serves the identical bytes.
    [[nodiscard]] result_t<view::view_t> read_identity() const;
    // ":children[]" read => member enumeration (write-spec / read-members asymmetry,
    // reference 05 §SPEC): a POINT whose children are POINT{NAME} member descriptors.
    // A vertex carrying handlers.on_children serves that synthesized listing instead
    // (ADR-0044 — a transport vertex lists live bus peers, no vertices created);
    // otherwise the direct child vertices registered under v's key are enumerated.
    [[nodiscard]] result_t<view::view_t> read_children(vertex_t* v) const;
    // ":acl" read => the stored ACEs RE-ENCODED (#81-A, ADR-0018/0020, #907) — a projection
    // of the list acl_allows walks. The READ_ACL gate runs in read(v, field, caller).
    [[nodiscard]] result_t<view::view_t> read_acl(vertex_t* v) const;
    // Bare ":settings" read (RFC-0010 §A.4 as amended by RFC-0022 §4) => the settings
    // container: the reserved `app` record iff a descriptor table is installed, and
    // NOTHING else — the core knob namespace is empty. An empty SETTINGS{} is the honest
    // answer for a vertex that declares no app fields.
    [[nodiscard]] result_t<view::view_t> read_settings(vertex_t* v) const;
    // ":settings.app" read (RFC-0010 §A.4) => the app container alone: declared,
    // non-`wo` fields that hold a value, in table order, values verbatim.
    // SCHEMA_NOT_FOUND when no table is installed (the closed default).
    [[nodiscard]] result_t<view::view_t> read_settings_app(vertex_t* v) const;

    // The full canonical key of `v` — its ancestors' NAME records concatenated root-down
    // (ADR-0057 render-on-demand: vertices store one segment, not the full key) — rendered
    // into `out`, whose source the bytes come from. Walks immutable parent links, so no lock.
    // Used only at sweep/observed-write/wiring frequency (the RFC-0008 byte-keyed sweep sets,
    // `create_child` key composition). The ONE renderer since #1778 (its throwing twin
    // `build_key` is gone): false when the source refused (out is then empty), so every
    // caller drops, defers or answers BACKPRESSURE instead of aborting.
    [[nodiscard]] static bool try_build_key(const vertex_t* v, mem::bytes_t& out) noexcept;
    // Bump every strict descendant's listeners_above_ by `delta` (RFC-0005 bookkeeping) —
    // a child-link subtree walk (placeholders included, so a later fill inherits a
    // correct count). Call with map_mutex_ held (shared suffices; counters are atomics).
    static void bump_subtree_listeners(vertex_t* v, std::int32_t delta);

    // RFC-0009 §B.6: pre-order re-virginize of @p v's subtree — unwind each vertex's
    // subscriber contribution to its descendants' listeners_above_, revert it to a
    // placeholder, and flip it unregistered. Parks each detached value-seam block
    // into @ref retired_seams_. Each vertex's subscriber slot table is MOVED into @p gone,
    // which the caller reserved one entry per visited vertex, so the walk cannot fail; the
    // caller gives each routed edge's link hold back and drops the tables once the locks are
    // released (#1816, #1778). Call with map_mutex_ held UNIQUE (it flips registered_ and
    // parks into retired_seams_, both map-lock-guarded).
    /** @brief The slot tables a retirement moved out of its vertices. */
    using gone_edges_t = mem::block_array_t<mem::block_array_t<subscriber_t>>;
    void retire_subtree(vertex_t* v, gone_edges_t& gone);

    // Value-seam blocks detached by retirement (RFC-0009 §B.6). A seam is read lock-free,
    // so a swapped-out block cannot be freed while a reader might still hold the old
    // pointer — it is PARKED here (ADR-0057 insert-only, applied to the seam). Kept on the
    // GRAPH, not per-vertex, so an app-field / leaf vertex pays zero extra bytes. Appended
    // only under map_mutex_ (unique). #576: the park's other end is the public collect(),
    // which frees a seam once no router frame open at its epoch is still inside. The graph's own
    // destructor is a growth backstop only, not a substitute: this member is declared BEFORE
    // map_mutex_ and roots_, so it destructs LAST — a seam whose destructor re-enters the
    // graph finds a half-destroyed object. Until collect() runs the size is peer-driven (one
    // per BUS-link connection teardown; a point-to-point teardown parks nothing, because the
    // identity vertex only gets an on_children when link->bus() != nullptr), plus one per
    // creator endpoint of every transport vertex destroyed — hence the public
    // parked_seam_count().
    //
    // A table-source array of the parked blocks (#1778), which also served the seams: `retire`
    // reserves room for its whole subtree BEFORE it changes anything, so parking itself cannot
    // fail. A link inside the seam would have avoided the reservation but cost 8 B on every
    // seam-bearing vertex, and a reader may still be reading every other byte of it.
    //
    // Each entry carries the QSBR epoch closed after its unpublish (`%detail_qsbr::advance`):
    // collect() frees it only once every router frame open at that moment has left.
    /** @brief One parked seam block and the epoch it was parked at. */
    struct parked_seam_t {
        value_handlers_t* seam;  /**< @brief The detached block. */
        std::uint32_t epoch = 0; /**< @brief Free once every participant is past this. */
    };
    struct seam_park_t {
        mem::block_array_t<parked_seam_t> seams; /**< @brief The parked blocks. */
        /** @brief An empty park drawing from @p src. */
        explicit seam_park_t(mem::block_source_t& src) noexcept : seams(src) {}
        seam_park_t(const seam_park_t&) = delete;
        seam_park_t& operator=(const seam_park_t&) = delete;
        /** @brief Frees every seam still parked. */
        ~seam_park_t() { free_all(seams); }
        /** @brief Free every block in @p a into the source @p a draws from. */
        static void free_all(mem::block_array_t<parked_seam_t>& a) noexcept {
            for (const parked_seam_t& s : a) mem::drop_in(a.source(), s.seam);
            a.clear();
        }
    };
    // The graph's OWN table sub-pool (#1778). A default graph on a `kSlabPool` build derives
    // one from the platform heap instead of sharing the host root's: with one pool for every
    // graph, independent graphs written from different threads paid for each other's blocks
    // (inproc-mt4 -28%, #1882). Declared FIRST, so it is built before and destroyed after
    // every member that draws from it. Empty on an injected root, which serves every purpose.
    // Defined in graph.cpp, so this header carries no pool definition into every unit that
    // includes it (with it, the forward router's inlining moved: +319 B, symbol ratchet).
    struct own_pool_t {
        mem::block_source_t* pool = nullptr; /**< @brief The pool, or null. */
        /** @brief Derive one when @p src is the default root of a slab-pool build. */
        explicit own_pool_t(mem::block_source_t& src) noexcept;
        ~own_pool_t();
        own_pool_t(const own_pool_t&) = delete;
        own_pool_t& operator=(const own_pool_t&) = delete;
        /** @brief The pool, or @p src when there is none. */
        [[nodiscard]] mem::block_source_t& or_root(mem::block_source_t& src) const noexcept {
            return pool != nullptr ? *pool : src;
        }
    };
    own_pool_t own_tables_;
    seam_park_t retired_seams_;

    // The node-scoped vertex index (RFC-0024 §6.4) — the ONE new structure a bound path
    // needs, named honestly. The "vertex map" is a Composite tree of non-moving unique_ptr
    // allocations with no dense index, so an element's u32 index has no meaning until one
    // exists. This is it: one slot appended per vertex_t ALLOCATION (slot 0 = roots_,
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
    // `anchor_root()` itself, which takes NO slot: it is a private parent, never registered,
    // never filled and never mintable, so the property this container actually owes RFC-0024
    // — "every vertex a mint may be asked for has exactly one immovable slot" — is untouched.
    //
    // CHUNKED, not a flat array, and the difference is the whole charged cost. A flat array
    // grows geometrically, so between two doublings it holds up to TWICE the pointers it
    // needs: measured on the 512-vertex heap probe (bench_forward_heap `zeroheap vertex`) a
    // vector cost 15 B/vertex live where the RFC charges 8 — the slack, not the slot, was
    // most of it. Fixed-size chunks make live bytes track the vertex count instead of the
    // last doubling: 8 B/vertex, exactly the pointer §6.4 prices. Indexing stays O(1) and
    // elements never move, which is all the deref needs.
    //
    // Its chunks and directory draw from the table source (#1778), and growth is split from
    // the append: `reserve_next` is the one failable step, taken BEFORE the vertex is
    // allocated, so `push_back` cannot fail and a refused creation leaves nothing behind.
    class vertex_index_t {
       public:
        /** @brief Slots per chunk — 512 B of pointers on a 64-bit host. */
        static constexpr std::size_t kChunk = 64;
        /** @brief An empty index drawing from @p src. */
        explicit vertex_index_t(mem::block_source_t& src) noexcept : dir_(src) {}
        vertex_index_t(const vertex_index_t&) = delete;
        vertex_index_t& operator=(const vertex_index_t&) = delete;
        /** @brief Returns every chunk. */
        ~vertex_index_t() {
            for (vertex_t** c : dir_)
                dir_.source().release(c, kChunk * sizeof(vertex_t*), alignof(vertex_t*));
        }
        /** @brief Slots appended so far. */
        [[nodiscard]] std::size_t size() const noexcept { return n_; }
        /** @brief Slot @p i (`i < size()`). */
        [[nodiscard]] vertex_t* operator[](std::size_t i) const noexcept {
            return dir_[i / kChunk][i % kChunk];
        }
        /** @brief Make room for one more slot. @retval false The source refused. */
        [[nodiscard]] bool reserve_next() noexcept {
            if (n_ < dir_.size() * kChunk) return true;
            // The chunk first, then the directory entry, which grows by doubling (an exact
            // `reserve(size() + 1)` regrew the directory on every chunk); a refused entry gives
            // the chunk back.
            void* const c = dir_.source().try_alloc(kChunk * sizeof(vertex_t*), alignof(vertex_t*));
            if (c == nullptr) return false;
            if (dir_.push_back(static_cast<vertex_t**>(c))) return true;
            dir_.source().release(c, kChunk * sizeof(vertex_t*), alignof(vertex_t*));
            return false;
        }
        /** @brief Append @p v into the room `reserve_next` made. */
        void push_back(vertex_t* v) noexcept {
            dir_[n_ / kChunk][n_ % kChunk] = v;
            ++n_;
        }

       private:
        mem::block_array_t<vertex_t**> dir_; /**< @brief The chunk directory. */
        std::size_t n_ = 0;                  /**< @brief Slots appended. */
    };
    vertex_index_t vertex_slots_;

    /**
     * @brief Stamp @p v with the index of the `vertex_slots_` entry that was just appended
     *        for it — the write half of the #1486 reverse memo.
     *
     * Called immediately after every `vertex_slots_.push_back`, under the SAME unique
     * `map_mutex_` hold that appended, and exactly once per `vertex_t` for the life of the
     * graph. The memo lives in bytes borrowed from `path_key_t` (`owner_slot_`, whose block
     * comment states whose bytes those are and why they had to live there); nothing
     * invalidates it, because a slot index is immortal — the index is insert-only and
     * pointer-stable (ADR-0057), and retirement re-virginizes a `vertex_t` IN PLACE rather
     * than renumbering it, which is why the generation, not the index, is the staleness
     * signal (RFC-0024 §6.4).
     *
     * There is no `graph_t` state here beyond the index itself and no public reader: the
     * memo is an implementation detail of @ref vertex_slot, which re-validates it against
     * the deque before trusting it.
     */
    void note_owner_slot(vertex_t& v) noexcept;

    /**
     * @brief A fresh handler-less placeholder named @p record, with its index slot reserved;
     *        null when the table source refuses the name, the slot or the vertex.
     *
     * The one construction both creating doors (the registration descent and
     * @ref register_session_anchor) share. Every draw comes from the injected table source,
     * the name's spill block included (#1991), and nothing is linked in, so the caller's
     * refusal leaves the tree as it was. Callers hold the unique `map_mutex_`.
     */
    [[nodiscard]] vertex_t* make_placeholder(std::span<const std::byte> record) noexcept;

    // ---- DECLARED FIRST so it is DESTROYED LAST (#873 phase 1) -----------------------
    //
    // The graph's internal face of the one injected source for VALUE segments, and its
    // position in the object is a LIFETIME requirement, not a preference: a payload segment a
    // vertex retains still reclaims through `src_backend_` from inside `~graph_t`, when
    // `roots_`'s vertex tree is torn down. Members are destroyed in REVERSE declaration order,
    // so an adapter declared after `roots_` is already dead by then: a virtual call on a
    // destroyed object, caught by UBSan's `vptr` check across six tests. Declaring it first
    // inverts that and is robust by construction. Its pmr sibling `src_mr_` is gone (#1778):
    // no graph table draws through `std::pmr` any more.

    /** @brief The graph's OWN `mem_backend_t` over the injected source (#873 phase 1).
     *
     *         Constructed unconditionally and pointed at by `value_backend_` only when
     *         a non-default source was injected — a process-default graph keeps
     *         @ref mem::heap_backend so its value path is provably the pre-#873 one, down
     *         to the ADR-0047 §2 devirtualized `HEAP` reclaim arm. Held BY VALUE: it is
     *         three words, and making it optional would cost the same space plus a branch. */
    mem::source_backend_t src_backend_;

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

    /** @brief The ADR-0060 byte-buffer seam the write-path copy-store draws its owned
     *         value @ref view::segment_t from (the flatten of a branch/field write,
     *         `graph.cpp` sites 825/1017), plus both folded READs' POINT headers (#831).
     *
     *         Since #873 phase 1 this is no longer injected either — it POINTS at
     *         @ref mem::heap_backend for a process-default graph and at the graph's own
     *         `src_backend_` wrapper over the injected source otherwise. Since phase 3 it is
     *         also where every READ-BACK encoder's segment comes from (published as
     *         @ref value_backend so the free functions in `graph.cpp` can name it), so the
     *         name is now narrower than the role: it is the graph's ONE segment seam, not
     *         only the write path's. On exhaustion
     *         it answers `nullptr` and the write BACKPRESSUREs (§3), never a silent heap
     *         fallback; thread-safety is the injected source's contract (§2), because a
     *         segment's reclaim self-routes on the last-ref thread. */
    mem::mem_backend_t* value_backend_ = &mem::heap_backend();
    /** @brief The two structural roots in ONE table-source block (#1778): the tree root and the
     *         session anchors' private root below. One owner keeps the pair at the two words the
     *         two `unique_ptr`s took, so no member after it moves. */
    struct roots_t {
        vertex_t root;   /**< @brief The Composite tree's root. */
        vertex_t anchor; /**< @brief The session anchors' private root (#1223). */
        /** @brief Both roots, their children drawing from @p src. */
        explicit roots_t(mem::block_source_t& src) noexcept
            : root(role_t::STORED_VALUE, path_key_t{}, handlers_t{}, src),
              anchor(role_t::STORED_VALUE, path_key_t{}, handlers_t{}, src) {}
    };
    mem::block_ptr_t<roots_t> roots_;
    /** @brief The tree root. */
    [[nodiscard]] vertex_t* root() const noexcept { return &roots_->root; }
    /** @brief The anchors' private root. */
    [[nodiscard]] vertex_t* anchor_root() const noexcept { return &roots_->anchor; }
    // The session identity anchors' private structural root (#1223, ADR-0044 §Amendment
    // 2026-08-13). NOT reachable from roots_ — that is the whole design: an anchor gets a
    // vertex-map slot and a saturating generation without becoming an ADDRESS. `find` and
    // every `:children[]` listing walk from roots_, so they never see one; a bus mount's
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
    // Anchor keys cannot collide with an addressable vertex's key: try_build_key stops at the
    // node whose parent is null, so an anchor's key is its own single NAME record, and the
    // router composes that record's content to contain `:` and `/` — two of the SEVEN
    // characters `path::valid_segment` rejects, so no registered address anywhere in this
    // graph can render the same bytes. That is what keeps retirement's sweep-set cleanup
    // (which is keyed by rendered key bytes) from ever touching a real vertex's entry.
    // (`anchor_root()`, the second half of `roots_` above.)
    // The device creation catalog (#82, ADR-0017): SPEC `type` -> factory. Populated at
    // setup (register_child_type) — configuration, per #1049's ruling, exactly like the
    // three sinks below. Looked up by string_view; drawn from the table source (#1778).
    //
    // It is a sorted table, so #1049's {fn, ctx} + slot mechanism does not reach it: there is
    // no pointer-sized word to publish, and the read walks entries an insert would be
    // moving. The lookup runs from a PEER's bytes (create_child) and is control-plane
    // cold — one vertex creation — so the arm that costs nothing where it lands is a lock,
    // and it is taken here rather than on any hot path. It buys a defined outcome for a
    // caller that violates the setup-only contract instead of a corrupted tree walk.
    mutable std::shared_mutex child_types_mutex_;
    mem::sorted_map_t<mem::string_t, child_factory_t> child_types_;
    // The five CONFIGURATION sinks (#1049). Each is the ADR-0047 {fn, ctx} pair published
    // through a sink_slot_t — the mechanism #914 established for fwd_router_t's five, hoisted
    // to the layer-neutral `tr` namespace so L4 can hold one without naming the net plane.
    // The doctrine is setup-only and `set_hooks` says so; the slot is
    // what makes a violation DEFINED (a skipped dispatch) rather than the use-after-free the
    // std::function predecessors had, since assigning a std::function destroys the old
    // target — freeing its captures while a reader is inside the call. An unset slot reads as
    // one relaxed load, which is what the null check on the plain member cost.
    tr::sink_slot_t<remote_delivery_fn_t> remote_sink_;  // read on the write hot path
    // The ACL gate reads ONLY `subject_lookup_` (#1781). A resolver installed in the returning
    // form is kept in `subject_resolver_` and reached through `lookup_via_resolver`, which
    // `set_hooks` installs in its place — so the disabled gate stays one relaxed load.
    tr::sink_slot_t<subject_lookup_fn_t> subject_lookup_;       // read by the ACL gate
    tr::sink_slot_t<subject_resolver_fn_t> subject_resolver_;   // the returning form, if set
    tr::sink_slot_t<sub_observer_fn_t> subscription_observer_;  // read on subscribe/clear
    tr::sink_slot_t<wire_target_fn_t> wire_target_;             // read on a wire subscribe only
    // The FIFTH (RFC-0010 Amendment 2): the net plane's `:stats` seam sampler, installed by
    // the router's constructor beside the other five it runs there. Read ONLY inside the
    // already-cold `:stats` field-read path, so it is off every hot path by construction —
    // its cost to a node that never reads the census is the three words it occupies.
    tr::sink_slot_t<stats_sampler_fn_t> stats_sampler_;  // read on a `:stats` read only
    // The routed-subscription hold (#1816): read only where a remote edge is admitted or
    // reclaimed, never on the write or delivery path.
    tr::sink_slot_t<link_hold_fn_t> link_hold_;
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
    // anywhere that matters. Same reasoning as child_types_ above: a byte array has no
    // pointer-sized word for the slot mechanism to publish. Drawn from the table source
    // (#1778); install and clear SWAP under the lock and free after it, so no allocator call
    // runs inside this leaf.
    //
    // The reader copies these bytes out through a stack buffer and allocates only after
    // unlocking, so this mutex is a strict LEAF — acquired at three sites, holding nothing
    // else, calling no allocator. That is what keeps it free of a lock-ordering obligation
    // once #873 injects a `block_source_t` (whose `Sync` policy may take its own lock) at
    // the allocation site.
    mutable std::shared_mutex identity_mutex_;
    mem::bytes_t identity_record_;
    /**
     * @brief The two test/bench instrumentation counters, present only when
     *        @ref default_config_t::kInstrumentCounters is bound `true` (#1664).
     *
     * Both this and `no_instrument_counters_t` expose the same four members, so the two
     * increment sites and the two accessors are written once and the lean build's calls
     * inline to nothing.
     */
    struct instrument_counters_t {
        // 64-bit on purpose (#1697): only a test or bench build binds this, never an MCU
        // image, so the rv32 libatomic call these would be is never linked.
        /** @brief Bubbling-walk instrumentation (RFC-0005) — see ancestor_walks(). */
        std::atomic<std::uint64_t> walks{0};
        /** @brief Canonical-fallback instrumentation (#830) — see target_canonical_resolves().
         *         Touched only when a target edge has no usable binding, so the bound leg
         *         pays nothing. */
        std::atomic<std::uint64_t> resolves{0};
        /** @brief Count one ancestor walk. */
        void tick_walk() noexcept { walks.fetch_add(1, std::memory_order_relaxed); }
        /** @brief Count one canonical-fallback resolve. */
        void tick_resolve() noexcept { resolves.fetch_add(1, std::memory_order_relaxed); }
        /** @brief Ancestor walks so far. */
        [[nodiscard]] std::uint64_t walk_count() const noexcept {
            return walks.load(std::memory_order_relaxed);
        }
        /** @brief Canonical-fallback resolves so far. */
        [[nodiscard]] std::uint64_t resolve_count() const noexcept {
            return resolves.load(std::memory_order_relaxed);
        }
    };
    /** @brief The closed-out stand-in: empty, so `[[no_unique_address]]` costs no bytes, and
     *         every member is a no-op that answers `0`. */
    struct no_instrument_counters_t {
        /** @brief Nothing to count. */
        void tick_walk() noexcept {}
        /** @brief Nothing to count. */
        void tick_resolve() noexcept {}
        /** @brief Always `0`: the counter is compiled out. */
        [[nodiscard]] static constexpr std::uint64_t walk_count() noexcept { return 0; }
        /** @brief Always `0`: the counter is compiled out. */
        [[nodiscard]] static constexpr std::uint64_t resolve_count() noexcept { return 0; }
    };
    /** @brief This build's counters — zero bytes and zero increments on a lean build. */
    [[no_unique_address]] mutable std::conditional_t<kInstrumentCounters, instrument_counters_t,
                                                     no_instrument_counters_t> instrument_;
    // Per-cause delivery-drop instrumentation — see delivery_drops(). Touched only on the
    // drop path, so the delivering path is byte-identical while nothing drops. Word-wide
    // storage behind the 64-bit snapshot (core/STYLE.md §Introspection clause 5, #1697): a
    // 64-bit atomic is a libatomic call on every rv32, the ESP32-C6 included, and costs 4 B of
    // alignment there. A 32-bit target wraps a cause after 2^32 drops; a 64-bit host is
    // unchanged.
    mutable std::atomic<std::size_t> drops_no_target_{0};
    mutable std::atomic<std::size_t> drops_denied_{0};
    mutable std::atomic<std::size_t> drops_oom_{0};
    mutable std::atomic<std::size_t> drops_truncated_{0};

    // The propagate-sweep selection sets (RFC-0008 §B), keyed on canonical PATH-payload
    // bytes and ORDERED so a subtree is a contiguous prefix range (a parent's key is a
    // byte-prefix of every descendant's — key_view_t::is_ancestor_of). `pending_` holds
    // the IF_NEWER vertices assigned since the last covering sweep (drained on sweep);
    // `unconditional_` holds every UNCONDITIONAL vertex (persistent membership, iterated
    // not drained). Both guarded by sweep_mutex_ — a distinct, coarse lock touched only
    // when a subscriber exists at/above a written vertex, so the idle write stays
    // lock-free. Snapshots are taken under it and delivered outside it (callbacks /
    // re-dispatch re-enter the graph), mirroring fan_out's discipline.
    //
    // Sorted tables of table-source keys since #1778, in leaves of at most 64 entries since
    // #1886: an insert or a single erase moves at most one leaf (a whole-array table moved the
    // tail, O(N) under this graph-wide lock on every marking assign), and the subtree range is
    // still one run in key order, found by binary search.
    key_set_t pending_;
    key_set_t unconditional_;
    std::mutex sweep_mutex_;
    // pending_.size() mirrored as a relaxed atomic: the observed-write fast path
    // (clear_pending on every eager delivery) skips the key render + sweep lock while no
    // assign has marked anything — losing a race with a concurrent mark_pending leaves the
    // mark for the next sweep, an ordering the locked erase already permitted (ADR-0057).
    // The per-vertex pending-mark hint (`vertex_t` flag, #1712) is the finer gate in front of
    // it: an unmarked vertex skips the path even while OTHER vertices hold marks.
    // Word-wide, so its RMW is one `amoadd.w` on rv32imac (#1697). Every writer holds
    // sweep_mutex_, so on a core with no atomic RMW (ESP32-C3) a load + store would do in
    // place of the libatomic call. It stays an RMW: that spelling re-partitions graph.cpp's
    // inline budget (`snapshot_edges` +39 B on the symbol ratchet), and on the C3 the call
    // runs inside a section that already takes a mutex.
    std::atomic<std::size_t> pending_count_{0};
    /** @brief The #551 nothrow failable-block seam — and, since #873 phase 1, THE source:
     *         the one the constructor was handed, from which every other channel is built.
     *
     *         Defaults to `mem::default_root()` (the host slab pool, or the static arena on an MCU
     * build), so behaviour is byte-identical until a host injects a bounded source — except that
     * exhaustion is a `nullptr` return rather than the `-fno-exceptions` abort stub.
     *
     *         LAST on purpose: no hot path reads it, so declaring it here keeps
     *         every other member at the byte offset it had before this seam
     *         existed. A cold pointer inserted mid-object would shift `roots_` and
     *         everything after it, which is a layout change the forward-hop bench
     *         can see and nothing gains from.
     *
     *         The adapter built over this source — `src_backend_` — does NOT sit here for
     *         that reason: they are declared FIRST in the class (see the
     *         "DECLARED FIRST so they are DESTROYED LAST" block), because a retained payload
     *         segment reclaims through `src_backend_` while `roots_`'s vertex tree is torn
     *         down, and members die in reverse declaration order. Moving them down here to
     *         match this member's layout argument reintroduces the UBSan `vptr` lifetime
     *         bug that placement fixed.
     */
    mem::block_source_t* ctl_ = &mem::heap_source();

    /** @brief The VALUE sub-pool (#1777): every published value, the graph-level DEFAULT
     *         receiver-ring admissions (RFC-0025 §4.6.1 clause 3) and a router's warm COMPACT
     *         copy draw from it.
     *
     *         The root's value sub-pool on a default graph, and the injected root itself
     *         otherwise, so every graph-level byte still draws from the one store the host
     *         passed. It replaced `ring_`, which was an alias of `ctl_` (#1822, folded into
     *         #1777): rings draw from the derived layout like the values they hold. There is
     *         still no graph-level ring SEAM (#1581, 2026-09-29): divergence is per vertex,
     *         through @ref vertex_policy_t::ring_source (bound by `vertex_t::set_ring_source`)
     *         — receiver-pays, so a receiver that must not be affected by another's exhaustion
     *         brings its own source, and per-vertex isolation stays a tested property.
     *
     *         Declared beside `ctl_` for the reason that member documents — a cold pointer
     *         inserted mid-object shifts `roots_` and every hot member after it. */
    mem::block_source_t* values_ = &mem::heap_source();

    /** @brief The TABLE sub-pool (#1777): registration, the control-plane containers and the
     *         failable scratch of a composed read or a branch write draw from it. The root's
     *         table sub-pool on a default graph, the injected root otherwise. Declared beside
     *         `ctl_` for the reason that member documents. */
    mem::block_source_t* tables_ = &mem::heap_source();

    /** @brief The @ref set_vertex_ceiling bound, charged against `vertex_slots_.size()` (#1314).
     *
     * Atomic rather than map-lock state: the charge is read on the creation path, which already
     * holds the unique map lock, but the deployer sizes and observes it from anywhere. Declared
     * beside `ctl_` for the reason that member documents — a cold word inserted mid-object shifts
     * `roots_` and every hot member after it. */
    std::atomic<std::size_t> vertex_ceiling_{kNoVertexCeiling};
    /** @brief Creations the ceiling refused — the evidence the bound bit (#838's count-then-act).
     *         Word-wide for the reason the delivery-drop counters are (#1697). */
    mutable std::atomic<std::size_t> vertex_ceiling_refusals_{0};

    // ---- #1071: the per-link departure index. LAST, beside `ctl_`, and for the same
    // reason that member documents: no hot path reads it, so declaring it here keeps
    // `roots_`, `vertex_slots_` and every other member at the byte offset it had before this
    // index existed. Declared mid-object instead, it shifted those offsets and
    // `graph_t::fan_out` grew 48 B of wider displacement encodings — a codegen change on the
    // DELIVERY path, which the symbol ratchet caught and which nothing here gains from.
    /**
     * @brief Which vertices hold a subscriber edge for a given link name (#1071) — the index
     *        behind the link doors above and the two evictions. Its design, its carry and its
     *        locking are documented on the type (`link_index.hpp`, #1710).
     *
     * Every table it keeps draws from the graph's table source (#1778).
     */
    link_index_t link_index_{*tables_};

    /**
     * @brief One vertex's declared payload-type → required-ACL-right rows (RFC-0014 Am. 2),
     *        as a node of the graph's insert-only, immortal declaration list.
     */
    struct payload_right_node_t {
        /** @brief An empty node drawing from @p src. */
        explicit payload_right_node_t(mem::block_source_t& src) noexcept
            : rows(src), catalog(src) {}
        const vertex_t* v = nullptr;              /**< @brief The declaring vertex. */
        mem::block_array_t<payload_right_t> rows; /**< @brief Its table, in declaration order. */
        /** @brief Its declared `:schema` catalog (RFC-0014 Amendment 3) — the encoded content
         *         of the schema's `SETTINGS`, served verbatim; empty when it declared none. It
         *         rides this node because it is the same kind of declaration (what a CONTROL
         *         vertex's writes accept), made at the same moment by the same caller. */
        mem::bytes_t catalog;
        payload_right_node_t* next = nullptr; /**< @brief The previously declared node. */
    };

    /**
     * @brief Head of the payload-right declaration list — the RFC-0014 Amendment 2 rows for
     *        every vertex that declared any, NEWEST FIRST.
     *
     * **Why the rows are here and not on the vertex.** The declaration is control-plane data
     * on a handful of control vertices, and every candidate per-vertex home charges the
     * vertices that declare nothing: the value-seam block is allocated for any vertex with a
     * seam (a bus link's `on_children` identity vertex, every handler), the app-field group
     * for every owner-declared field table, and `vertex_ext_t` for all of them. Measured, a
     * `std::vector` on the seam block is **+16 B on every handler-bearing vertex** — the
     * `reg_escape` memory probe catches it — for a feature those vertices do not use. That is
     * exactly the trade ADR-0058 made when it split the seam block out of `vertex_t`, applied
     * one level further, so it is made the same way.
     *
     * **Insert-only and immortal, so the read is lock-free.** Nodes are PREPENDED under the
     * unique map lock at registration and are never removed or freed until the graph itself is
     * destroyed: the
     * write gate walks the list with no lock, and a node it is reading can never be recycled
     * under it. Retirement does not unlink — it clears the vertex's `PAYLOAD_RIGHTS` flag,
     * which is what stops the gate looking at all, and a re-registration that declares again
     * prepends NEWER rows that the walk therefore finds first (ADR-0057's insert-only
     * discipline, the same one the seam park keeps).
     *
     * **What it costs the rest of the graph:** two members here, and one relaxed flag-bit test
     * on the write path. A node that declares nothing allocates nothing and never walks.
     */
    /**
     * @brief An insert-only, immortal declaration list that OWNS its nodes (#1778): the atomic
     *        head readers walk, plus the source every node came from.
     *
     * Owning through its own destructor is what keeps `graph_t` without a user-provided one.
     * (It measured: an out-of-line `~graph_t` re-partitioned GCC's inline budget in this TU and
     * grew `dispatch_edge_remote` by 32 B — the #1223/#1250 hazard, caught by the symbol
     * ratchet.) Nodes are prepended under the unique map lock and freed only here.
     */
    template <class N>
    struct immortal_list_t {
        std::atomic<N*> head{nullptr};      /**< @brief Newest node first. */
        mem::block_source_t* src = nullptr; /**< @brief Where every node came from. */
        /** @brief An empty list. */
        immortal_list_t() noexcept = default;
        immortal_list_t(const immortal_list_t&) = delete;
        immortal_list_t& operator=(const immortal_list_t&) = delete;
        /** @brief Frees every node. */
        ~immortal_list_t() {
            for (N* n = head.load(std::memory_order_relaxed); n != nullptr;) {
                N* const next = n->next;
                mem::drop_in(*src, n);
                n = next;
            }
        }
        /** @brief Publish @p n as the newest node. Under the unique map lock. */
        void prepend(N* n) noexcept {
            n->next = head.load(std::memory_order_relaxed);
            head.store(n, std::memory_order_release);
        }
    };
    immortal_list_t<payload_right_node_t> payload_rights_;

    /** @brief Publish @p rows and @p catalog as @p v's declaration and set its flag. Call with
     *         `map_mutex_` held UNIQUE (the registration hold). A declaration with neither is
     *         ignored. @retval false The table source refused the node; nothing published. */
    [[nodiscard]] bool declare_payload_rights(vertex_t* v, std::span<const payload_right_t> rows,
                                              std::span<const std::byte> catalog);

    /** @brief @p v's declared `:schema` catalog bytes — empty unless it declared one.
     *         Lock-free; the caller has already tested the flag. */
    [[nodiscard]] std::span<const std::byte> declared_catalog(const vertex_t* v) const noexcept;

    /** @brief The right @p v demands for a written TLV of @p type — `WRITE` unless @p v
     *         declared a row for it. Lock-free; the caller has already tested the flag. */
    [[nodiscard]] acl_right_t declared_write_right(const vertex_t* v, wire::type_t type) const;

    /** @brief The creation-hook slot of a build without `config_t::kCreationHooks`: it holds
     *         nothing, an install stores nothing, and it reads back as the empty hook. */
    struct no_creation_hook_t {
        /** @brief Store nothing. */
        no_creation_hook_t& operator=(const creation_hook_t& /*hook*/) noexcept { return *this; }
        /** @brief Read back as the empty hook. */
        operator creation_hook_t() const noexcept {
            return {};
        }  // NOLINT(google-explicit-constructor)
    };
    /** @brief What an admission node holds for the creation hook: the hook itself when the
     *         build allows one, else an empty type that costs the node no bytes. */
    using creation_slot_t =
        std::conditional_t<config_t::kCreationHooks, creation_hook_t, no_creation_hook_t>;

    /**
     * @brief One vertex's ADMISSION filters (`handlers_t::on_admit` and
     *        `handlers_t::on_app_field_admit`), as a node of the graph's insert-only, immortal
     *        declaration list.
     */
    struct admission_node_t {
        const vertex_t* v = nullptr; /**< @brief The declaring vertex. */
        /** @brief The value plane's pre-store filter, or empty. */
        admit_hook_t on_admit;
        /** @brief The app-field plane's pre-store filter, or empty. */
        app_field_admit_hook_t on_app_field_admit;
        /** @brief The app-field plane's on-demand read seam (#1878), or empty. Not a filter,
         *         but owner control-plane data on the few vertices that install it, so it
         *         rides this node for the reason `%admissions_` states. */
        app_field_read_hook_t on_app_field_read;
        /** @brief The creation hook (RFC-0030 §7.2), or empty. Not a filter either, and it
         *         rides this node for the same reason. Zero bytes in a build without
         *         `config_t::kCreationHooks`. */
        [[no_unique_address]] creation_slot_t on_create{};
        admission_node_t* next = nullptr; /**< @brief The previously declared node. */
    };

    /**
     * @brief Head of the ADMISSION declaration list — the pre-store filters of every vertex
     *        that installed one, NEWEST FIRST.
     *
     * **Why the filters are here and not on the vertex**, which is the same question
     * `%payload_rights_` answers and the same answer, measured the same way. A filter is
     * control-plane data on the few vertices that defend an invariant; every per-vertex home
     * charges the vertices that install none. Two `std::function`s on the value-seam block cost
     * **+64 B on every seam-bearing vertex** (the `reg_escape` memory probe caught exactly
     * that), and one on the app-field group costs +32 B on every vertex that declares a field
     * (the `vertex_app5` probes caught that); as @ref hook_t pairs since RFC-0028 slice 7 it
     * would be half that, and half is still not nothing. Neither population is the one using the
     * feature. Off-vertex, a vertex that installs no filter pays one flag bit and nothing else, and
     * a vertex that installs one pays a single node here.
     *
     * **Insert-only and immortal, so the read is lock-free** — node lifetime, retirement and
     * re-registration all work exactly as `%payload_rights_` describes: prepended under the
     * unique map lock, never unlinked, and retirement clears the vertex's `ADMISSION` flag
     * rather than removing anything, so a newer declaration is simply found first.
     */
    immortal_list_t<admission_node_t> admissions_;

    /** @brief Publish @p h's two admission filters and its app-field read seam as @p v's node
     *         and set its flag. Call with `map_mutex_` held UNIQUE (the registration hold). A
     *         declaration with none of the three set is ignored.
     *         @retval false The table source refused the node. */
    [[nodiscard]] bool declare_admission(vertex_t* v, const handlers_t& h);

    /** @brief @p v's admission node, or null when it has none. Lock-free; one flag test for
     *         the vertices without one. */
    [[nodiscard]] const admission_node_t* admission_for(const vertex_t* v) const noexcept;

    /** @brief @p v's app-field read seam (`handlers_t::on_app_field_read`, #1878), or an empty
     *         hook when it installed none — one flag test for the vertices without one. */
    [[nodiscard]] app_field_read_hook_t app_field_reader(const vertex_t* v) const noexcept;

    /** @brief @p v's creation hook (RFC-0030 §7.2), or an empty hook when it carries none —
     *         one flag test for the vertices without one, and always empty in a build without
     *         `config_t::kCreationHooks`. */
    [[nodiscard]] creation_hook_t creation_hook_for(const vertex_t* v) const noexcept;

    /**
     * @brief The releases @ref park_release parked: each runs once, at the first @ref collect
     *        that no router frame open at its epoch is still inside. Appended only under
     *        `map_mutex_` (unique), so epochs grow along the array.
     *
     * Declared LAST, away from `retired_seams_`, for two reasons. Its size moves no member the
     * write and fan-out paths address. And it destructs FIRST, so a release still parked when
     * the graph goes runs against a graph that is still whole.
     */
    /** @brief One parked release and the epoch it was parked at (see `parked_seam_t`). */
    struct parked_release_t {
        retired_callback_t release; /**< @brief Run once, at the first ripe collect(). */
        std::uint32_t epoch = 0;    /**< @brief Ripe once every participant is past this. */
    };
    struct release_park_t {
        mem::block_array_t<parked_release_t> releases; /**< @brief The parked releases. */
        /** @brief An empty park drawing from @p src. */
        explicit release_park_t(mem::block_source_t& src) noexcept : releases(src) {}
        release_park_t(const release_park_t&) = delete;
        release_park_t& operator=(const release_park_t&) = delete;
        /** @brief Runs every release still parked. */
        ~release_park_t() {
            for (const parked_release_t& c : releases) c.release.release(c.release.ctx);
            releases.clear();
        }
    };
    release_park_t parked_releases_; /**< @brief See `release_park_t`. */
};

namespace detail_qsbr {

/**
 * @brief One read-side bracket of the QSBR domain on this thread: `fwd_router_t`'s frame
 *        bracket, and `transport_vertex_t::with_link`'s.
 *
 * Nests; only the outermost announces, and its exit is a quiescent state
 * (`graph_t::thread_quiescent`, a no-op unless `reclaim_qsbr` is bound).
 */
struct frame_scope_t {
    frame_scope_t() noexcept { enter(); }
    frame_scope_t(const frame_scope_t&) = delete;
    frame_scope_t& operator=(const frame_scope_t&) = delete;
    ~frame_scope_t() {
        if (leave()) graph_t::thread_quiescent();
    }
};

}  // namespace detail_qsbr

}  // namespace tr::graph
