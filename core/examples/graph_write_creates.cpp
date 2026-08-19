/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — write-creates: a LOCAL data write materializes its target.
 *
 * A local data write to an address that does not resolve creates the vertex and every
 * missing intermediate, `mkdir -p` style (RFC-0005 §D; `graph_t::ensure_vertex`). The
 * asymmetry is deliberate and is NOT shown here because it needs a peer: a REMOTE fieldless
 * `FWD{WRITE}` to an unresolved `dst` answers `tr::path::not_found` and creates nothing
 * (RFC-0005 §D amendment 1) — a peer creates through the ADR-0059 creator endpoint.
 *
 * The `:` control plane does not create either: a field write to a nonexistent vertex is
 * `NOT_FOUND`, because there is no vertex to control (`core/src/graph.cpp`, the field arm of
 * `write(const path_t&, rope_t)`). That half IS checked here.
 *
 * Runs under ctest as `example_graph_write_creates`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::status_t;

/** @brief An owned one-segment view over @p text. */
tr::view::view_t value_of(std::string_view text) {
    return *tr::view::over_bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
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

    // Nothing is registered. One write builds /zone, /zone/a and /zone/a/soil.
    check(ok, !g.find(path_t("/zone/a/soil").key()), "the target does not exist yet");
    const auto w = g.write(path_t("/zone/a/soil"), value_of("moist"));
    check(ok, w.has_value(), "a local data write to an unresolved path succeeds");
    check(ok, g.find(path_t("/zone/a/soil").key()).has_value(), "the target vertex now exists");
    check(ok, g.find(path_t("/zone/a").key()).has_value(),
          "the missing intermediate was created too");

    const auto rb = g.read(path_t("/zone/a/soil"));
    check(ok, rb.has_value(), "the created vertex serves the value that created it");

    // The intermediate is created but never written: it resolves, and has no value.
    const auto mid = g.read(path_t("/zone/a"));
    check(ok, mid.has_value(), "the intermediate reads as a composed branch, not NOT_FOUND");

    // Field writes never create — there is no vertex to control.
    const auto fw = g.write(path_t("/zone/b:acl"), value_of("x"));
    check(ok, !fw && fw.error() == status_t::NOT_FOUND,
          "a :field write to a nonexistent vertex is NOT_FOUND");
    check(ok, !g.find(path_t("/zone/b").key()), "and it created nothing");
    return ok ? 0 : 1;
}
