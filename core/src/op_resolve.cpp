/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/op_resolve.hpp"

#include "op_resolve_walk.hpp"

namespace tr::graph {

result_t<rope_t> op_resolver_t::resolve(const tlv_arena_t& fwd, std::string_view inbound_link,
                                        const view_t* frame_view) {
    // The span-tier instantiation: the arena root (index 0) read through the
    // node-reader concept. Byte-identical to the pre-templating resolver.
    //
    // The seam is per-CALL and lives on this stack frame (#801), exactly as the rope tier's
    // does: every node of the walk points at it, so the injected backend reaches the one
    // allocating site this tier has — `arena_node::own_wire`, the ADR-0041 §2 ownership copy
    // — one call below the router. Its `refused` half stays false here by construction: an
    // arena span is borrowed and cannot be shortened by a refusal, so the copy answers by
    // value (see `arena_node::spans_intact`). Its lifetime covers the nodes', which never
    // outlive `resolve_node`.
    const flatten_seam_t seam{.backend = flat_, .refused = false};
    return resolve_node(graph_, arena_node{&fwd, 0, &seam}, inbound_link, frame_view);
}

}  // namespace tr::graph
