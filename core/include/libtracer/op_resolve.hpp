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
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path_ref.hpp"
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
     * A non-empty @p inbound_link makes an inbound `:subscribers[]` WRITE bind a
     * REMOTE subscriber (#136): the slot retains this request's accumulated return
     * route (`src`, copied once — trailer-sliced) and @p inbound_link, so the
     * producer fan-out delivers a `FWD{WRITE}` / auto-promoted `COMPACT` back over
     * that link (RFC-0004 §D/§E.1). An empty @p inbound_link is the local-only
     * field-write — so `fwd_router_t`, which knows the link, passes it; a bare
     * local resolve does not.
     *
     * @p inbound_link is also the operation's ACL caller context (#81, ADR-0018):
     * every graph call the terminus makes passes it through, so with a subject
     * resolver installed a denied op replies `kind=ERROR` with
     * `STATUS{ERROR{VALUE tr::access::denied}}` (0x0050).
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
     * @param inbound_link This node's NAME for the link the request arrived on
     *                     (empty ⇒ local resolution, no remote-subscriber binding).
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
        const wire::tlv_arena_t& fwd, std::string_view inbound_link = {},
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
     * @param inbound_link This node's NAME for the link the request arrived on
     *                     (empty ⇒ local resolution, no remote-subscriber binding).
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
        const wire::tlv_view_t& fwd, std::string_view inbound_link = {},
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

   private:
    graph_t& graph_;
    mem::mem_backend_t* flat_ = &mem::heap_backend();    // rope-tier terminus flattens (#766)
    mem::mem_backend_t* egress_ = &mem::heap_backend();  // reply head + mint egress bytes (#795)
    reverse_ref_fn_t reverse_ref_fn_ = nullptr;  // responder's reverse-mint seam (amendment 1)
    void* reverse_ref_ctx_ = nullptr;            /**< @brief Its caller-owned context. */
    path_label_fn_t path_label_fn_ = nullptr;    // RFC-0027 §6.1 point 3 terminus mint seam
    void* path_label_ctx_ = nullptr;             /**< @brief Its caller-owned context. */
};

}  // namespace tr::graph
