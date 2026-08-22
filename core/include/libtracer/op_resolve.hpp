/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 — local operation resolution + the zero-copy FWD{REPLY}
 * builder, over the ADR-0041 terminus arena. Given an arena-decoded FWD whose
 * `dst` names a LOCAL vertex on this node, apply the op (READ / WRITE / AWAIT)
 * plus any FIELD `:field` selector against the graph and build the FWD{REPLY}
 * as a rope: one exactly-sized, direct-emitted head segment (op=REPLY, dst=the
 * request's src, src=this node's responder endpoint, kind) prepended to
 * refcount-clones of the vertex's stored payload view(s) — never a
 * flatten/serialize into a fresh buffer.
 *
 * The resolver reads the frame through arena spans (ADR-0041 §2 borrowed-span
 * contract): dispatch fields are raw span reads; the vertex lookup is
 * span-aliased for a canonical PATH (§3); the ownership copies (stored WRITE
 * value, reply route bytes, remote-subscriber return route) copy the node's
 * trailer-excluded `wire` span exactly once, trailer-less at rest (§4).
 *
 * Local-only: a `dst` that does not resolve to a local vertex (a transport
 * child or an unknown path) replies kind=ERROR STATUS=ERROR(NOT_FOUND);
 * hop-by-hop forwarding is fwd_router_t's. The L4 seam (the router becoming
 * transport-aware) lives here so the resolver and the transport plane share
 * one dispatch.
 */
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/peer_handle.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"
#include "libtracer/tlv_arena.hpp"
#include "libtracer/tlv_view.hpp"

namespace tr::graph {

/** @brief The four FWD operations (RFC-0004 §B — the `op` child, a u8). */
enum class fwd_op_t : std::uint8_t {
    READ = 0,  /**< @brief Read the data LKV or the selected `:field`. */
    WRITE = 1, /**< @brief Write the payload TLV to the vertex or the selected `:field`. */
    AWAIT = 2, /**< @brief Block for the next write (honoring `await_timeout`). */
    REPLY = 3, /**< @brief A reply routed back; not resolvable as a request here. */
};

/**
 * @brief The opcode field of the `FWD` `op` byte — bits 5-0 (RFC-0024 §7.5).
 *
 * Bits 7-6 are FLAGS. A forwarder MUST mask before switching (RFC-0024 §9.3): an
 * unrecognised flag then degrades to the plain opcode instead of an unknown-opcode reject,
 * which is what makes a flag additive at all. Four of the 64 opcode values are in use.
 */
inline constexpr std::uint8_t kFwdOpcodeMask = 0x3F;

/**
 * @brief `op` bit 7 — the bound-path MINT REQUEST (RFC-0024 §7.5).
 *
 * Set by an origin asking each host on the route to answer with its own vertex ref, so the
 * origin can address the same target in the bound form next time. It costs **zero request
 * bytes**: there is no free TLV `opt` bit (all six defined bits are assigned and the two
 * reserved ones make a frame invalid if set), and a dedicated presence child would spend a
 * 4-byte TLV header to express one bit.
 *
 * The request is a HINT, never an obligation: a host that will not or cannot mint (its
 * generation has saturated, it does not implement the amendment) answers the ordinary reply
 * and the origin stays on the canonical form, which always works.
 */
inline constexpr std::uint8_t kFwdOpFlagMintRequest = 0x80;

/** @brief The reply discriminant carried by a `FWD{REPLY}` (RFC-0004 §D). */
enum class reply_kind_t : std::uint8_t {
    RESULT = 0, /**< @brief Success — payload is the result (or empty for a WRITE ack). */
    ERROR = 1,  /**< @brief Failure — payload is `STATUS{ ERROR u8 }` (RFC-0002 model). */
};

/** @brief Default AWAIT deadline when a FWD carries no `await_timeout` child. */
inline constexpr std::chrono::nanoseconds kDefaultAwaitTimeout = std::chrono::seconds(1);

/**
 * @brief The inbound identity of a resolved operation: WHERE it arrived and WHO sent it
 *        (#375 Part 2 / #1266, fused — the 2026-08-16 ruling on #375).
 *
 * Two claims, and ADR-0082 is the reason they are two fields rather than one string:
 *
 *  - @ref link is ADDRESSING. It is this node's NAME for the link (or bus peer) the request
 *    arrived over, and it is what a remote subscription's deliveries route back through. It
 *    has always been the resolver's `inbound_link` and its meaning is unchanged.
 *  - @ref peer is IDENTITY. It is the opaque per-peer handle the transport minted at accept
 *    (#1294) and the peer-receiver seam tags each frame with — eight bytes, register-passed,
 *    carried instead of re-supplying a name string per frame. The operation's ACL SUBJECT is
 *    derived FROM it, at the terminus, through @ref op_resolver_t::on_peer_subject.
 *
 * Before the fusion these were the same `std::string_view`, which is exactly what made a
 * per-writer subject unreachable at `peer_named=false`: one FLAT link has one name for every
 * peer on it. Splitting them is what lets the subject differ per writer while the return
 * route stays the link's.
 *
 * The converting constructor from a `std::string_view` is deliberate and load-bearing for
 * compatibility: `resolve(fwd, "up")` still means what it always did — no handle, so the
 * subject IS the link name, byte for byte the pre-fusion behaviour.
 */
struct inbound_ref_t {
    /** @brief This node's NAME for the link the request arrived on; empty ⇒ LOCAL
     *         resolution (no remote-subscriber binding, the trusted local ACL door). */
    std::string_view link;
    /** @brief The interned per-peer identity of the writer (`tr::net::peer_handle_t`,
     *         #1294). Not `valid()` ⇒ this kind supplied none, and the subject falls back
     *         to @ref link. */
    net::peer_handle_t peer;
    /**
     * @brief An OPAQUE token the subject supplier interprets — never dereferenced here.
     *
     * A handle is meaningful only to the link that minted it, so resolving one to a subject
     * needs to know WHICH link. The router passes its own per-child receive context through
     * this field and reads it back inside its supplier; every other caller leaves it null.
     * It is a `const void*` because `tr::graph` is L4 and may not name a transport type
     * (core/STYLE.md — dependencies point up the layers only).
     */
    const void* origin = nullptr;

    /** @brief A resolve with no peer identity: the subject is the link name itself. */
    constexpr inbound_ref_t() noexcept = default;
    /**
     * @brief Implicit from a bare link NAME — the pre-#375-Part-2 spelling, unchanged.
     *
     * Templated over anything a `std::string_view` is constructible from, rather than taking
     * one directly, because a `resolve(fwd, "up")` would otherwise need TWO user-defined
     * conversions (`const char[3]` → `string_view` → here) and stop compiling. The point of
     * the implicit door is that no existing caller has to change; a door every literal misses
     * is not that door.
     */
    template <class S>
        requires std::constructible_from<std::string_view, const S&> &&
                 (!std::same_as<std::remove_cvref_t<S>, inbound_ref_t>)
    constexpr inbound_ref_t(const S& inbound_link) noexcept  // NOLINT(*-explicit-*)
        : link(inbound_link) {}
    /** @brief The full form the router builds: a link name, its frame's peer, and the
     *         opaque token its subject supplier resolves that peer against. */
    constexpr inbound_ref_t(std::string_view inbound_link, net::peer_handle_t inbound_peer,
                            const void* supplier_origin) noexcept
        : link(inbound_link), peer(inbound_peer), origin(supplier_origin) {}
};

/**
 * @brief The terminus's LINK-TOKEN supplier: `(ctx, the inbound identity) → the link's
 *        interned `%tr::graph::link_id_t`` (#1266 / #1417).
 *
 * Declared at namespace scope rather than inside `op_resolver_t` — unlike its three sibling
 * seams — because @ref link_token_seam_t below has to name it, and that struct is what the
 * walk carries. `op_resolver_t::on_link_id` installs it.
 *
 * The supplier is expected to ANSWER FROM A CACHE it filled at link-up, not to compute
 * anything: it is asked on the subscribe path, where the whole point is to spend a subscript
 * instead of a hash. A default-constructed answer means "no token", which the index handles
 * by interning the name exactly as it does today.
 *
 * @param ctx     The caller-owned context handed to `op_resolver_t::on_link_id`.
 * @param inbound The request's inbound identity — `peer` is the handle whose link is wanted
 *                and `origin` is the opaque per-link token the installer put there.
 */
using link_id_fn_t = link_id_t (*)(void* ctx, const inbound_ref_t& inbound);

/**
 * @brief The link-token seam as the resolve walk carries it — the `{fn, ctx}` pair plus the
 *        identity it resolves.
 *
 * Bundled deliberately. The walk already threads two bare `{fn, ctx}` pairs and a third would
 * make `resolve_node` and `apply_op` grow THREE parameters (the pair, plus the
 * @ref inbound_ref_t the supplier needs and the walk otherwise flattens away into a bare
 * link name). One trivially-copyable three-word struct with a "not installed" default keeps
 * every existing call site and every existing default argument exactly where it was.
 *
 * @ref inbound points into the resolve's own stack frame and never outlives it.
 */
struct link_token_seam_t {
    link_id_fn_t fn = nullptr;              /**< @brief The supplier, or null. */
    void* ctx = nullptr;                    /**< @brief Its caller-owned context. */
    const inbound_ref_t* inbound = nullptr; /**< @brief What to resolve; null ⇒ no token. */

    /**
     * @brief Ask for the token — the ONE place the seam is consulted.
     *
     * Called from the subscribe branch alone, so an un-installed seam costs one predictable
     * branch on a path that is already building an owned SUBSCRIBER copy, and an installed
     * one costs an indirect call per remote subscribe and nothing per frame.
     */
    [[nodiscard]] link_id_t ask() const {
        if (fn == nullptr || inbound == nullptr) return {};
        return fn(ctx, *inbound);
    }
};

/**
 * @brief Resolves an arena-decoded FWD against a local graph and builds the FWD{REPLY} rope.
 *
 * Local-only (RFC-0004 / ADR-0035): no transport, no multi-hop forwarding, no
 * route-handle. Construct over the node's @ref graph_t; call @ref resolve once
 * per inbound request FWD, with the arena from `wire::decode_into` (ADR-0041).
 */
class op_resolver_t {
   public:
    /**
     * @brief Bind the resolver to the local @p graph it resolves `dst` against.
     *
     * @param graph The node's local graph.
     * @param flat  The byte backend the terminus draws its owned `segment`s from
     *              (#766, #793, #801) — the per-node contiguous-span materialize
     *              (`view_node::ensure_cache`, reached by every `wire()`/`body()` read of a
     *              multi-link TLV) and BOTH branches of the ownership copy
     *              (`view_node::own_wire`: the ADR-0053 ⑤ flatten of a straddling payload and
     *              the ADR-0041 §2 copy of a contiguous one — the latter reached the global
     *              heap through `view::over_bytes` until #793, so which allocator a write used
     *              depended on where the PEER's fragmentation happened to fall). These sit one
     *              call BELOW
     *              `fwd_router_t::resolve_terminus_rope`, and until #766 they took
     *              @ref mem::heap_backend unconditionally: a bounded node that pointed every
     *              other injection at its own slab still drew from the global heap the moment
     *              a peer sent a FRAGMENTED terminus request — peer-drivable, and an
     *              `abort()` under `-fno-exceptions`. `fwd_router_t` passes its own `flat`
     *              here, so one injection now covers the router's four sites AND the terminus.
     *
     *              The SPAN (arena) tier draws from it too since #801 — at one site,
     *              `arena_node::own_wire`, the ADR-0041 §2 ownership copy of a borrowed arena
     *              span. That is the whole of what this tier allocates (its `wire()`/`body()`
     *              spans are borrowed from the frame and never materialize), and it was the
     *              last ownership copy in either tier still going to `view::over_bytes`'s
     *              global heap. It is the MCU terminus's ORDINARY case, not an exotic one: a
     *              synchronous CAN/UART child delivers a contiguous span, so every WRITE it
     *              carries took the unbounded copy. A refusal is answered by value, through
     *              the empty-view BACKPRESSURE channel below — not through `spans_intact()`,
     *              which stays a constant `true` on this tier because a borrowed span cannot
     *              be shortened by a refused allocation.
     *
     *              The default is the global heap, so every existing call site is unchanged.
     *
     *              A refused flatten is answered BY VALUE, never by reading a short span: the
     *              resolve walk carries a per-call "spans intact" flag (`spans_intact()` on the
     *              node-reader concept) and turns a refusal into an addressed `kind=ERROR`
     *              `STATUS{BACKPRESSURE}` reply — or, when the refusal hit the reply's OWN
     *              route bytes and no trustworthy address is left, into a `BACKPRESSURE`
     *              status on the error side, which the router drops. Never a truncated reply.
     *
     *              An injected @p flat MUST be thread-safe on the same terms `fwd_router_t`
     *              documents for its own: the terminus resolves on a transport child's receive
     *              thread and several children receive concurrently. Must outlive the
     *              resolver.
     * @param egress The byte backend the FWD{REPLY}'s EGRESS-construction segments draw from
     *              (#795, ADR-0074) — the reply head (peer-driven size: the swapped route bytes
     *              plus the inline tail) and, on a mint, the trailing 12-byte `PATH_REF`. It is
     *              the last reply-egress byte source a bounded node could not previously bound
     *              (both folded READs' POINT-header framing — composed-root and `":children"` —
     *              is payload framing and draws from the graph's value seam instead: #831,
     *              closed): the
     *              head was hard-wired to `view::heap_alloc`'s global heap regardless of every
     *              other injection. A DEDICATED seam, not `flat`: `flat` is documented and sized
     *              against FLATTEN (payload) bytes, and folding an egress head into it would
     *              silently re-scope a budget deployments already set. The default is the global
     *              heap, so every existing call site is byte-unchanged; a bounded node points it
     *              at its own slab and this allocation joins the bound. A refusal returns an empty
     *              rope that `or_backpressure` turns into an addressed `kind=ERROR`
     *              `STATUS{BACKPRESSURE}` — exhaustion answered by value, never an abort. MUST be
     *              thread-safe on the same terms as @p flat. Must outlive the resolver.
     */
    explicit op_resolver_t(graph_t& graph, mem::mem_backend_t* flat = &mem::heap_backend(),
                           mem::mem_backend_t* egress = &mem::heap_backend()) noexcept
        : graph_(graph), flat_(flat), egress_(egress) {}

    /**
     * @brief Resolve an arena-decoded request FWD and build the zero-copy `FWD{REPLY}` rope.
     *
     * The op-level outcome (NOT_FOUND for a non-local `dst`, INVALID_PATH for a
     * `[*]` wildcard on a non-subscriber path, TIMEOUT for an AWAIT, …) is encoded
     * as a `kind=ERROR` reply on the value side — a built reply, not a failure.
     * The error side is reserved for a structurally malformed FWD (not a FWD, or
     * missing the required `op`/`dst`/`src` children) that no reply can describe,
     * and for a REPLY frame (which is routed, not resolved, here).
     *
     * **A request whose `src` is a ZERO-LENGTH `PATH` is UNACKNOWLEDGED** (RFC-0004
     * Amendment 2, #1502): the return route is also the acknowledgement request, and
     * the empty route is the request not made. A `WRITE` is applied and answers with
     * an **empty rope** — success, refusal and ACL denial alike, since none of them
     * has anywhere to go. A `READ`, an `AWAIT`, a mint-flagged frame or a
     * `:subscribers[]` subscribe is MALFORMED on that route and refuses on the error
     * side. The empty rope is therefore no longer only the egress-exhaustion signal
     * it was; callers already treat it as "nothing to send" (`fwd_router_t`'s two
     * terminus arms), and this is a second, deliberate reason for it.
     *
     * A non-empty `inbound.link` makes an inbound `:subscribers[]` WRITE bind a
     * REMOTE subscriber (#136): the slot retains this request's accumulated return
     * route (`src`, copied once — trailer-sliced) and `inbound.link`, so the
     * producer fan-out delivers a `FWD{WRITE}` / auto-promoted `COMPACT` back over
     * that link (RFC-0004 §D/§E.1). An empty `inbound.link` is the local-only
     * field-write — so `fwd_router_t`, which knows the link, passes it; a bare
     * local resolve does not.
     *
     * The operation's ACL caller context (#81, ADR-0018) is the SUBJECT, which is
     * derived HERE, at the terminus, and threaded through every graph call the walk
     * makes — so with a subject resolver installed a denied op replies `kind=ERROR`
     * with `STATUS{ERROR{VALUE tr::access::denied}}` (0x0050). The derivation is
     * `inbound.peer` through the installed supplier (@ref on_peer_subject), falling
     * back to `inbound.link` when there is no valid handle, no supplier, or the
     * supplier declines — which is every caller that passes a bare link name, and is
     * byte for byte what the resolver did before the two claims were split.
     *
     * The arena (and the frame it borrows) only needs to outlive this call: every
     * span the reply retains is copied once to its owner (ADR-0041 §2) — or, on an
     * owning-delivery frame, referenced off it (ADR-0042 §3, below).
     *
     * A non-null @p frame_view marks the frame as OWNING (delivered as a
     * refcounted `view_t` over the same bytes the arena borrows — the ADR-0042
     * receiver seam). Then a WRITE whose payload TLV (`node.wire`) clears the
     * RFC-0022 §3.D amplification predicate `payload * K >= segment` — `K` being the
     * target vertex's owner-declared `pin_payload_ratio` when set and
     * `config_t::kPinPayloadRatio` otherwise — and whose opt byte carries no trailer
     * bits is stored as a SUBVIEW of the frame — a refcount
     * bump that pins the whole frame, zero copy. Segment-dominated, trailered, or
     * span-delivered payloads keep the ADR-0041 one-copy trailer-sliced store,
     * byte-identical to before; the remote-subscriber return route always keeps
     * its subscription-scoped one-copy behavior.
     *
     * @param fwd          An arena-decoded request FWD (from `wire::decode_into`).
     * @param inbound      WHERE the request arrived and WHO sent it (@ref inbound_ref_t) —
     *                     implicitly constructible from a bare link NAME, which is the
     *                     pre-#375-Part-2 spelling and behaves identically. An empty
     *                     `inbound.link` is a local resolution (no remote-subscriber
     *                     binding); a valid `inbound.peer` is what the installed subject
     *                     supplier (@ref on_peer_subject) derives the ACL subject from.
     * @param frame_view   The owning frame view when the link delivers views
     *                     (ADR-0042); nullptr on the borrowed-span path.
     * @param dst_label_target **RFC-0027 §7.2 at a terminus** — the vertex a LABELLED `dst`
     *                     already dereferenced to, or nullptr, which is every string- and
     *                     `PATH_REF`-spelled request and therefore every request a host with
     *                     no injected label table ever sees.
     *
     *                     A label REPLACES the string bytes of the part it stands for (§6.1),
     *                     so a labelled `dst` that reaches its terminus carries no name to
     *                     look up: `path_lookup_key` refuses an escape record in key context
     *                     and must, because reading a peer's slot index as UTF-8 is the
     *                     guessing §7.2 forbids. The address is resolved BEFORE this call, by
     *                     the transport plane that owns the label table
     *                     (`%tr::net::path_label_table_t`), and what arrives here is that
     *                     resolution: this node's own reference to the vertex the label
     *                     aliases — the identical element RFC-0024's bound spelling carries.
     *
     *                     Non-null means **resolve against this element and not against
     *                     `dst`'s bytes**, on the SAME arm the bound spelling takes: one
     *                     `deref_vertex_slot`, then the operation's own ACL gate inside
     *                     `graph_t::read` / `write` / `await` at the dereferenced vertex.
     *                     That reuse is how §8.2's *"exactly as the string form does"*
     *                     becomes one implementation instead of two kept in agreement. A
     *                     stale element — the vertex retired between the label's mint and
     *                     this frame — answers `NOT_FOUND` on the error side, which the
     *                     router turns into a drop, exactly as a stale `PATH_REF` does.
     *
     *                     Two MINTS are suppressed while it is set, and both are §11.2's rule
     *                     rather than an optimisation: this address is already spelled in one
     *                     compressed form, so the reply neither mints a second label into it
     *                     (there is nothing left to replace — the request's `dst`, which the
     *                     reply echoes as its `src`, IS the label) nor answers an RFC-0024
     *                     mint request with a `PATH_REF` for it (*"SHOULD NOT bind a
     *                     `PATH_REF` over a path whose elements are already labelled"*).
     * @return The reply as a @ref view::rope_t (head segment + roped payload views),
     *         or a `status_t` on a malformed/non-request frame.
     */
    [[nodiscard]] result_t<view::rope_t> resolve(
        const wire::tlv_arena_t& fwd, const inbound_ref_t& inbound = {},
        const view::view_t* frame_view = nullptr,
        const wire::path_ref_element_t* dst_label_target = nullptr);

    /**
     * @brief Resolve a rope-delivered request FWD (the lazy `tlv_view_t` tier) and
     *        build the `FWD{REPLY}` rope — the owning-delivery twin of the arena
     *        overload (ADR-0053 §7).
     *
     * The same terminus semantics as the @ref wire::tlv_arena_t overload — it runs
     * the ONE templated resolve walk, here over the forward-only @ref
     * wire::tlv_view_t reader (ADR-0053 §1), so a frame reassembled as a
     * scatter-gather rope (fragmented WS / CAN) is resolved WITHOUT an interim
     * flatten of the whole frame. Byte-identical replies to the arena tier for the
     * same logical request (the differential oracle in `op_resolve_view_test`).
     *
     * @param fwd          A rope-backed request FWD (`wire::tlv_view_t::over`).
     * @param inbound      WHERE the request arrived and WHO sent it, exactly as the arena
     *                     overload documents (@ref inbound_ref_t).
     * @param frame_view   Reserved for the ADR-0042 owning-store seam; the rope
     *                     tier stores its one ownership copy, so pass `nullptr`.
     * @param dst_label_target The RFC-0027 §7.2 labelled-`dst` resolution, with exactly the
     *                     meaning and the two suppressed mints the arena overload documents
     *                     at length. Both tiers take it because a labelled request may
     *                     arrive fragmented like any other, and the two tiers answering one
     *                     logical request differently is the drift ADR-0053 §7's single walk
     *                     exists to make impossible.
     * @return The reply as a @ref view::rope_t, or a `status_t` on a
     *         malformed/non-request frame.
     */
    [[nodiscard]] result_t<view::rope_t> resolve(
        const wire::tlv_view_t& fwd, const inbound_ref_t& inbound = {},
        const view::view_t* frame_view = nullptr,
        const wire::path_ref_element_t* dst_label_target = nullptr);

    /**
     * @brief The responder's own reverse-direction element supplier (RFC-0024 §7.1
     *        amendment 1): asked, at most once per mint-flagged remote subscribe, for THIS
     *        node's reference to the connection vertex @p inbound_link arrived on.
     *
     * The mapping from a link NAME to its connection vertex is the transport plane's
     * (`fwd_router_t`'s receiver contexts), which this graph-layer resolver deliberately
     * cannot name — so it is injected as a bare `{fn, ctx}` pair, the ADR-0047 seam shape.
     * `nullopt` (or no installed supplier — every pre-amendment embedder) means the
     * responder cannot complete the reverse list: the subscription stores none and stays
     * canonical-only, the documented degrade.
     */
    using reverse_ref_fn_t =
        std::optional<wire::path_ref_element_t> (*)(void* ctx, std::string_view inbound_link);

    /** @brief Install the reverse-element supplier (null @p fn uninstalls). */
    void on_reverse_ref(reverse_ref_fn_t fn, void* ctx) noexcept {
        reverse_ref_fn_ = fn;
        reverse_ref_ctx_ = ctx;
    }

    /**
     * @brief The SUBJECT supplier (#375 Part 2 / #1266): asked, at most once per resolve and
     *        only for a request carrying a valid `inbound_ref_t::peer`, for the ACL subject
     *        token that peer writes under.
     *
     * This is where *"the subject is derived at the terminus from the handle"* actually
     * happens. The handle is opaque and the table that gives it meaning belongs to the
     * transport plane, which this graph-layer resolver deliberately cannot name — so the
     * derivation is injected as a bare `{fn, ctx}` pair, the ADR-0047 seam shape
     * @ref on_reverse_ref and @ref on_path_label already use. `fwd_router_t` installs one
     * that forwards to `transport_t::peer_subject` on the link the frame arrived over.
     *
     * Called at most ONCE per resolve, before any graph call, and never for a local resolve:
     * the derived token is then the caller context for every gate the walk runs — the READ
     * gate, the WRITE gate, the SUBSCRIBE gate and the `graph::write_ctx_t` a HANDLER sees —
     * so a handler and the `:acl` that admitted its write cannot disagree.
     *
     * **An EMPTY answer is the conformant default, not an error**: the subject falls back to
     * `inbound_ref_t::link`, i.e. exactly the caller context the resolver used before the
     * split. Every embedder with no installed supplier is that case, so no shipped
     * deployment's gate decisions move until a link starts minting per-peer subjects.
     *
     * @param ctx     The caller-owned context handed to @ref on_peer_subject.
     * @param inbound The request's inbound identity — `peer` is the handle to resolve and
     *                `origin` is the opaque token the installer put there.
     * @param scratch At least `%tr::net::kPeerNameChars` bytes of storage the token may be
     *                formatted into; it outlives the whole resolve, so the returned view may
     *                point into it.
     */
    using subject_fn_t = std::string_view (*)(void* ctx, const inbound_ref_t& inbound,
                                              std::span<char> scratch);

    /** @brief Install the subject supplier (null @p fn uninstalls). */
    void on_peer_subject(subject_fn_t fn, void* ctx) noexcept {
        subject_fn_ = fn;
        subject_ctx_ = ctx;
    }

    /**
     * @brief The TERMINUS's path-label supplier (RFC-0027 §6.1 point 3): asked, on a
     *        successful operation only, for the label element standing for the residual this
     *        node just resolved.
     *
     * §6.1 point 3 — *"the terminus does the same for the residual it resolved"* — is the
     * residual half of the rewrite whose forwarding half rides `reply_label`. The reply's
     * `src` IS the request's `dst` (the residual), so the terminus's rewrite is a
     * SUBSTITUTION of that one region: the label REPLACES the string bytes and never appends,
     * exactly as §6.1 requires in both directions, and it lands in the one region that
     * survives to the origin (erratum 2).
     *
     * What is injected and why: a label is minted against a `peer_handle_t` out of a table
     * the TRANSPORT plane owns (`%tr::net::path_label_table_t`), and this graph-layer resolver
     * deliberately cannot name either. So the mapping from "the link this request arrived on"
     * plus "the vertex it resolved to" to "seven encoded bytes" is injected as a bare
     * `{fn, ctx}` pair — the ADR-0047 seam shape `on_reverse_ref` already uses.
     *
     * The contract each side holds:
     *
     * - **Post-auth, always** (§8.1). The supplier is called only after the operation's own
     *   gates have passed — after `graph_t::read` / `write` / `await` / `subscribe_wire`
     *   answered success — so no label is ever minted for a destination an ancestor ACL
     *   hides, and a denied operation answers denied and nothing else.
     * - **An EMPTY answer is the conformant default**, not an error (§6.3): the reply's `src`
     *   stays the string it is today. Every host with no installed supplier is that case, so
     *   no shipped reply's bytes move.
     * - The returned span must be exactly `%tr::wire::kPathLabelRecordBytes` long and must
     *   outlive the reply assembly; anything else is ignored and the part stays a string.
     */
    using path_label_fn_t = std::span<const std::byte> (*)(void* ctx, std::string_view inbound_link,
                                                           wire::path_ref_element_t target);

    /** @brief Install the terminus path-label supplier (null @p fn uninstalls). */
    void on_path_label(path_label_fn_t fn, void* ctx) noexcept {
        path_label_fn_ = fn;
        path_label_ctx_ = ctx;
    }

    /**
     * @brief Install the terminus LINK-TOKEN supplier (null @p fn uninstalls) — #1266 /
     *        #1417's carry.
     *
     * The seam that lets a remote subscribe reach the subscriber index by SUBSCRIPT instead
     * of by name hash. The supplier is the transport plane's: it minted the link's
     * `graph_t::intern_link` token when the link (or the bus peer) became audible and cached
     * it in its own per-link receive context, which is what @ref inbound_ref_t::origin points
     * at. Worth 42–56 % of the index operation net of its control, and 27 % of its bytes.
     *
     * ASKED LAZILY, at the subscribe branch and nowhere else. That is the load-bearing part
     * of the contract: a control-plane saving paid on every terminus frame is what killed
     * #1290's prototype, so unlike @ref subject_fn_t this one is NOT resolved once per
     * resolve. A read, a write, an await and a forwarding hop never call it.
     *
     * An invalid answer is the conformant default and costs nothing but the lookup the index
     * does today — no installed supplier, a peer the supplier has no token for, a census bus
     * that never announced. So is a WRONG answer: the index verifies that the token's slot
     * spells the key it is about to index under, so a supplier that confuses two links loses
     * a subscript, not an edge.
     */
    void on_link_id(link_id_fn_t fn, void* ctx) noexcept {
        link_id_fn_ = fn;
        link_id_ctx_ = ctx;
    }

   private:
    graph_t& graph_;
    mem::mem_backend_t* flat_ = &mem::heap_backend();    // rope-tier terminus flattens (#766)
    mem::mem_backend_t* egress_ = &mem::heap_backend();  // reply head + mint egress bytes (#795)
    reverse_ref_fn_t reverse_ref_fn_ = nullptr;  // responder's reverse-mint seam (amendment 1)
    void* reverse_ref_ctx_ = nullptr;            /**< @brief Its caller-owned context. */
    path_label_fn_t path_label_fn_ = nullptr;    // RFC-0027 §6.1 point 3 terminus mint seam
    void* path_label_ctx_ = nullptr;             /**< @brief Its caller-owned context. */
    subject_fn_t subject_fn_ = nullptr;          // #375 Part 2 terminus subject derivation
    void* subject_ctx_ = nullptr;                /**< @brief Its caller-owned context. */
    link_id_fn_t link_id_fn_ = nullptr;          // #1417 terminus link-token carry
    void* link_id_ctx_ = nullptr;                /**< @brief Its caller-owned context. */

    /** @brief The token seam as the walk carries it — the pair plus the identity it
     *         resolves, bundled so `resolve_node` grows ONE parameter and not three. */
    [[nodiscard]] link_token_seam_t link_token_seam(const inbound_ref_t& inbound) const noexcept {
        return link_token_seam_t{.fn = link_id_fn_, .ctx = link_id_ctx_, .inbound = &inbound};
    }

    /**
     * @brief Derive the operation's ACL subject from @p inbound — the ONE place the split
     *        between *where it arrived* and *who sent it* is collapsed back to a token.
     *
     * Shared by both `resolve` overloads so the two tiers cannot drift (ADR-0053 §7's rule
     * for the walk, applied to its caller context). Every guard below degrades to
     * `inbound.link`, which is what the resolver has always used.
     */
    [[nodiscard]] std::string_view subject_for(const inbound_ref_t& inbound,
                                               std::span<char> scratch) const {
        if (subject_fn_ == nullptr || !inbound.peer.valid()) return inbound.link;
        const std::string_view s = subject_fn_(subject_ctx_, inbound, scratch);
        return s.empty() ? inbound.link : s;
    }
};

}  // namespace tr::graph
