/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the data plane: `write` then `read`, and the one store per vertex.
 *
 * A `STORED_VALUE` vertex is last-writer-wins: a write replaces the stored value, a read
 * serves the latest one, and a vertex that was never written has no last-known-value at all
 * (`NOT_FOUND`, distinct from an address that does not resolve — both spell `NOT_FOUND`
 * here, `docs/reference/02-graph-model.md` §Vertex lifecycle). `read` hands back a
 * `value_ref_t` — a REFERENCE to the published value, not a copy of it.
 *
 * Runs under ctest as `example_graph_read_write`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;

/** @brief An owned one-segment view over @p text — a VALUE's bytes are opaque to L4. */
tr::view::view_t value_of(std::string_view text) {
    return *tr::view::over_bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

/** @brief True iff @p v's bytes equal @p text. */
bool holds(const tr::view::view_t& v, std::string_view text) {
    return v.bytes().size() == text.size() &&
           std::memcmp(v.bytes().data(), text.data(), text.size()) == 0;
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

    const auto temp = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    const auto unwritten = g.read(temp);
    check(ok, !unwritten && unwritten.error() == status_t::NOT_FOUND,
          "a never-written vertex has no last-known-value");

    (void)g.write(temp, value_of("21.5C"));
    const auto first = g.read(temp);
    check(ok, first && holds((*first)->only(), "21.5C"), "read serves what was written");

    // Last-writer-wins: the second write REPLACES the store; there is one store per vertex.
    (void)g.write(temp, value_of("22.0C"));
    const auto second = g.read(temp);
    check(ok, second && holds((*second)->only(), "22.0C"),
          "a second write replaces the stored value");

    // The first read still holds its own reference — the value it names stays alive.
    check(ok, holds((*first)->only(), "21.5C"), "an outstanding value_ref_t keeps its value alive");
    std::printf("/sensor/temp now reads %.*s\n", static_cast<int>((*second)->only().bytes().size()),
                reinterpret_cast<const char*>((*second)->only().bytes().data()));
    return ok ? 0 : 1;
}
