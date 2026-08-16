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
    // The injected backend reaches the one allocating site this tier has —
    // `arena_node::own_wire`, the ADR-0041 §2 ownership copy — one call below the router
    // (#801). It travels as an ARGUMENT, not as a node member: `arena_node` is copied by
    // value throughout the walk, so keeping it two words wide costs nothing to choose and a
    // seam member could only cost more (see `arena_node::own_wire`). No `refused` flag on
    // this tier: an arena span is borrowed and cannot be shortened by a refusal, so the copy
    // answers by value through the empty view (see `arena_node::spans_intact`).
    // `egress` is the reply head + mint seam (#795, ADR-0074), injected alongside `flat`; the
    // default is the global heap, so an un-injected span-tier resolve is byte-unchanged.
    return resolve_node(graph_, arena_node{&fwd, 0}, inbound_link, frame_view,
                        flat_ != nullptr ? *flat_ : mem::heap_backend(),
                        egress_ != nullptr ? *egress_ : mem::heap_backend(), reverse_ref_fn_,
                        reverse_ref_ctx_, path_label_fn_, path_label_ctx_);
}

}  // namespace tr::graph
