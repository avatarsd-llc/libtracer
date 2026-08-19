/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — register a vertex, and address it by its canonical PATH bytes.
 *
 * A `path_t` parses its string ONCE into the canonical PATH-TLV payload (packed segment
 * records); the vertex map is keyed on those bytes, never on the string
 * (`docs/reference/02-graph-model.md` §Dispatch keyed on canonical PATH TLV bytes). Two
 * further first-contact facts fall out and are checked here: re-registering a live path is
 * `PATH_IN_USE`, and registering `/sensor/temp` does NOT register `/sensor` — the
 * intermediate is an unregistered structural placeholder (`CONTEXT.md` §Structural vertex).
 *
 * Runs under ctest as `example_graph_register`; returns non-zero on any failed check.
 */

#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

}  // namespace

int main() {
    tr::graph::graph_t g;
    bool ok = true;

    const path_t temp("/sensor/temp");
    const vertex_handle_t vh = g.register_vertex(temp, role_t::STORED_VALUE);
    std::printf("registered /sensor/temp — key is %zu packed bytes\n", temp.key().size());

    // find() takes the KEY, not the string: the same lookup the dispatcher runs.
    const auto found = g.find(temp.key());
    check(ok, found && *found == vh, "find(key) resolves to the registered handle");

    // register_vertex(literal) hard-aborts on a collision (ADR-0056: a source bug); the
    // fallible form is for a genuine runtime path, and reports the collision.
    const auto dup = g.try_register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    check(ok, !dup && dup.error() == status_t::PATH_IN_USE,
          "re-registering a live path is PATH_IN_USE");

    check(ok, !g.find(path_t("/sensor").key()),
          "the intermediate /sensor stays an unregistered placeholder");
    check(ok, !g.find(path_t("/sensor/humidity").key()), "an unknown path resolves to nothing");
    return ok ? 0 : 1;
}
