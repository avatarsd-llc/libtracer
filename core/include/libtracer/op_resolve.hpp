/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0004 / ADR-0035 slice 2 — local operation resolution + the zero-copy
 * FWD{REPLY} builder. Given a decoded FWD (the wire frame, tr::wire) whose `dst`
 * names a LOCAL vertex on this node, apply the op (READ / WRITE / AWAIT) plus any
 * FIELD `:field` selector against the graph and build the FWD{REPLY} as a rope:
 * a small freshly built head (op=REPLY, dst=the request's src, src=this node's
 * responder endpoint, kind) prepended to refcount-clones of the vertex's stored
 * payload view(s) — never a flatten/serialize into a fresh buffer.
 *
 * Slice 2 is local-only: a `dst` that does not resolve to a local vertex (a
 * transport child or an unknown path) replies kind=ERROR STATUS=ERROR(NOT_FOUND);
 * hop-by-hop forwarding is slice 3. The L4 seam (the router becoming transport-
 * aware) lives here so the resolver and the transport plane share one dispatch.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "libtracer/frame.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/status.hpp"

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
 * @brief Resolves a decoded FWD against a local graph and builds the FWD{REPLY} rope.
 *
 * Local-only (RFC-0004 / ADR-0035 slice 2): no transport, no multi-hop
 * forwarding, no route-handle. Construct over the node's @ref graph_t; call
 * @ref resolve once per inbound request FWD.
 */
class op_resolver_t {
   public:
    /** @brief Bind the resolver to the local @p graph it resolves `dst` against. */
    explicit op_resolver_t(graph_t& graph) noexcept : graph_(graph) {}

    /**
     * @brief Resolve a decoded request FWD and build the zero-copy `FWD{REPLY}` rope.
     *
     * The op-level outcome (NOT_FOUND for a non-local `dst`, INVALID_PATH for a
     * `[*]` wildcard on a non-subscriber path, TIMEOUT for an AWAIT, …) is encoded
     * as a `kind=ERROR` reply on the value side — a built reply, not a failure.
     * The error side is reserved for a structurally malformed FWD (not a FWD, or
     * missing the required `op`/`dst`/`src` children) that no reply can describe,
     * and for a REPLY frame (which is routed, not resolved, here).
     * @param fwd A decoded FWD TLV (`tr::wire`), e.g. from `decode`/`view_as_tlv`.
     * @return The reply as a @ref view::rope_t (head segment + roped payload views),
     *         or a @ref status_t on a malformed/non-request frame.
     */
    [[nodiscard]] result_t<view::rope_t> resolve(const wire::tlv_t& fwd) {
        return resolve(fwd, std::string_view{});
    }

    /**
     * @brief Resolve as @ref resolve, with the inbound link for remote-subscriber binding.
     *
     * Identical to the single-argument overload except that an inbound `:subscribers[]`
     * WRITE binds a REMOTE subscriber (#136): the slot retains this request's accumulated
     * return route (`src`) and @p inbound_link, so the producer fan-out delivers a
     * `FWD{WRITE}` / auto-promoted `COMPACT` back over that link (RFC-0004 §D/§E.1). An
     * empty @p inbound_link reproduces the local-only field-write (the slice-2 path) — so
     * `fwd_router_t`, which knows the link, passes it; a bare local resolve does not.
     *
     * @param fwd          A decoded request FWD TLV.
     * @param inbound_link This node's NAME for the link the request arrived on (empty ⇒
     *                     local resolution, no remote-subscriber binding).
     */
    [[nodiscard]] result_t<view::rope_t> resolve(const wire::tlv_t& fwd,
                                                 std::string_view inbound_link);

   private:
    graph_t& graph_;
};

}  // namespace tr::graph
