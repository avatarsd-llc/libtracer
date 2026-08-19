/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a STREAM vertex keeps a bounded history ring, depth declared owner-side.
 *
 * A `STORED_VALUE` vertex is last-writer-wins: *k* writes leave one value. A `STREAM` vertex
 * additionally appends each write to a bounded ring, and its contract is "observe every
 * buffered entry" rather than "the latest" (`docs/reference/02-graph-model.md` §Stream drain
 * semantics). The depth is a RETENTION INTENT only the application can supply, so it is an
 * owner-side call with **no wire surface at all** — `set_history_depth`, not a `:settings`
 * knob (RFC-0022 §3.C; the withdrawn `history_keep_last` reads `SCHEMA_NOT_FOUND`).
 *
 * A plain `read` is unchanged: it still serves the latest value, never the ring.
 *
 * Runs under ctest as `example_graph_stream`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief An owned one-segment view over @p text. */
tr::view::view_t value_of(std::string_view text) {
    return *tr::view::over_bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

/** @brief True iff @p r's single view holds @p text. */
bool holds(const tr::view::rope_t& r, std::string_view text) {
    return r.only().bytes().size() == text.size() &&
           std::memcmp(r.only().bytes().data(), text.data(), text.size()) == 0;
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

    const auto events = g.register_vertex(path_t("/dev/can0/rx"), role_t::STREAM);
    g.set_history_depth(events, 3);  // owner-side retention intent; no peer can read or write it

    for (const char* frame : {"f1", "f2", "f3", "f4"}) (void)g.write(events, value_of(frame));

    const auto ring = g.history(events);
    check(ok, ring.has_value(), "a STREAM vertex serves its history ring");
    std::printf("ring holds %zu of the 4 writes\n", ring ? ring->size() : 0u);
    check(ok, ring && ring->size() == 3,
          "the ring is bounded at the declared depth (oldest trimmed)");
    check(ok, ring && holds(ring->front(), "f2"), "history is oldest-first, and f1 was trimmed");
    check(ok, ring && holds(ring->back(), "f4"), "history is newest-last");

    const auto latest = g.read(events);
    check(ok, latest && holds(**latest, "f4"),
          "a plain read still serves the latest value, not the ring");
    return ok ? 0 : 1;
}
