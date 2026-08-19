/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — retirement: leaving the graph is emptying a vertex, not erasing it.
 *
 * `retire` marks a vertex and its whole subtree logically absent (RFC-0009 §A.1/§B): the
 * path then reads `NOT_FOUND`, identical to never-existed — there is no distinct `retired`
 * status. The vertex OBJECT is not freed, so an outstanding `vertex_handle_t` stays
 * dereferenceable (ADR-0057, insert-only); what tells a holder its cached resolution went
 * stale is `retire_generation`, which is bumped by the retirement (ADR-0062).
 *
 * Retirement delivers nothing and wakes no `await` (§B.5): a composite subscriber is never
 * told a child went away, so disappearance is observable only by re-reading
 * (`docs/reference/02-graph-model.md` §Retirement notification).
 *
 * Runs under ctest as `example_graph_retire`; returns non-zero on any failed check.
 */

#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;
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

    const auto air = g.register_vertex(path_t("/zone/air"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/zone/air/humidity"), role_t::STORED_VALUE);
    (void)g.write(air, value_of("ok"));
    const std::uint32_t gen_before = g.retire_generation(air);

    check(ok, g.retire(air).has_value(), "retire succeeds");
    check(ok, !g.find(path_t("/zone/air").key()), "the retired path no longer resolves");
    check(ok, !g.find(path_t("/zone/air/humidity").key()), "retirement takes the whole subtree");

    const auto r = g.read(path_t("/zone/air"));
    check(ok, !r && r.error() == status_t::NOT_FOUND,
          "a retired path reads NOT_FOUND, exactly like never-existed");
    check(ok, g.retire_generation(air) != gen_before,
          "the handle stays usable, and its generation moved");
    check(ok, g.retire(air).has_value(),
          "retiring an already-retired vertex is a no-op, not an error");

    // A later LOCAL write revives the address; the revived vertex inherits nothing (§B.6).
    check(ok, g.write(path_t("/zone/air"), value_of("fresh")).has_value(),
          "write-creates revives a retired address");
    std::printf("generation %u -> %u across one retirement\n", gen_before,
                g.retire_generation(air));
    return ok ? 0 : 1;
}
