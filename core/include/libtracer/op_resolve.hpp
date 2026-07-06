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
#include <cstdint>
#include <string_view>

#include "libtracer/graph.hpp"
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
    /** @brief Bind the resolver to the local @p graph it resolves `dst` against. */
    explicit op_resolver_t(graph_t& graph) noexcept : graph_(graph) {}

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
     * receiver seam). Then a WRITE whose payload TLV (`node.wire`) is at least the
     * target vertex's `settings.store_ref_min_bytes` (> 0) and whose opt byte
     * carries no trailer bits is stored as a SUBVIEW of the frame — a refcount
     * bump that pins the whole frame, zero copy. Smaller, trailered, or
     * span-delivered payloads keep the ADR-0041 one-copy trailer-sliced store,
     * byte-identical to before; the remote-subscriber return route always keeps
     * its subscription-scoped one-copy behavior.
     *
     * @param fwd          An arena-decoded request FWD (from `wire::decode_into`).
     * @param inbound_link This node's NAME for the link the request arrived on
     *                     (empty ⇒ local resolution, no remote-subscriber binding).
     * @param frame_view   The owning frame view when the link delivers views
     *                     (ADR-0042); nullptr on the borrowed-span path.
     * @return The reply as a @ref view::rope_t (head segment + roped payload views),
     *         or a `status_t` on a malformed/non-request frame.
     */
    [[nodiscard]] result_t<view::rope_t> resolve(const wire::tlv_arena_t& fwd,
                                                 std::string_view inbound_link = {},
                                                 const view::view_t* frame_view = nullptr);

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
     * @return The reply as a @ref view::rope_t, or a `status_t` on a
     *         malformed/non-request frame.
     */
    [[nodiscard]] result_t<view::rope_t> resolve(const wire::tlv_view_t& fwd,
                                                 std::string_view inbound_link = {},
                                                 const view::view_t* frame_view = nullptr);

   private:
    graph_t& graph_;
};

}  // namespace tr::graph
