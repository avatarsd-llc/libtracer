/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — enumerate a parent's members with the `:children[]` field read.
 *
 * `:children[]` is the `:` control plane, addressed WHOLE: the read serves a structured
 * (`PL=1`) reply whose children are the parent's registered members — members, not creation
 * SPECs (`docs/reference/02-graph-model.md` §Observing structural change). It enumerates one
 * level; a grandchild is read from its own parent, because each vertex answers for itself.
 *
 * The `:` plane is silent by design, so nothing here is a notification: a consumer watching
 * for structural change subscribes to the parent and re-enumerates (`CONTEXT.md`
 * §Announce write).
 *
 * Runs under ctest as `example_graph_children`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief The member count of a `:children[]` read, or `SIZE_MAX` when it did not resolve. */
std::size_t members(tr::graph::graph_t& g, const char* where) {
    const auto r = g.read(path_t(where));
    if (!r) return static_cast<std::size_t>(-1);
    // A composed reply is a rope, not one contiguous view: materialize before decoding
    // (single-link ⇒ a refcount bump, multi-link ⇒ the one flatten copy, ADR-0053 §6).
    const auto tlv = tr::wire::decode((*r)->materialize());
    return tlv ? tlv->children.size() : static_cast<std::size_t>(-1);
}

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    tr::graph::graph_t g;
    bool ok = true;

    (void)g.register_vertex(path_t("/zone"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/soil"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/air"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/air/humidity"), role_t::STORED_VALUE);

    std::printf("/zone:children[] lists %zu members\n", members(g, "/zone:children[]"));
    check(ok, members(g, "/zone:children[]") == 2, ":children[] enumerates ONE level (soil, air)");
    check(ok, members(g, "/zone/air:children[]") == 1,
          "a grandchild is enumerated from its own parent");
    check(ok, members(g, "/zone/soil:children[]") == 0,
          "a leaf enumerates an empty member list, not an error");
    return ok ? 0 : 1;
}
