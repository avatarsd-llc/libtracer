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
    return resolve_node(graph_, arena_node{&fwd, 0}, inbound_link, frame_view);
}

}  // namespace tr::graph
