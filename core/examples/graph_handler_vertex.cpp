/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a HANDLER vertex EXECUTES its write instead of storing it.
 *
 * The vertex's role decides what a write means at that address. `STORED_VALUE` assigns;
 * `HANDLER` (roles 3–7 of `docs/reference/11-vertex-roles-and-aggregation.md`) runs the
 * owner's `on_write`, and its `on_read` supplies a value the graph never held — an MMIO
 * register, a computation, a device command. The role is host state and appears on no wire:
 * a peer sees one address with `read`/`write`, exactly as for a stored value.
 *
 * The seam block is allocated on the PRESENCE of a handler, not on the role
 * (`core/include/libtracer/vertex.hpp`, `value_handlers_t`), so a `HANDLER` vertex
 * registered with an empty `handlers_t` allocates none.
 *
 * Runs under ctest as `example_graph_handler_vertex`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::handlers_t;
using tr::graph::path_t;
using tr::graph::role_t;

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
    int commands = 0;

    handlers_t relay;
    relay.on_write = [&commands](const tr::view::rope_t& in,
                                 const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        ++commands;  // the device acts here; nothing is assigned to the vertex
        std::printf("  [relay] command #%d, %zu bytes\n", commands, in.total_length());
        return {};
    };
    relay.on_read = [&commands]() -> tr::graph::result_t<tr::view::rope_t> {
        return tr::view::rope_t{value_of(commands % 2 ? "ON" : "OFF")};  // computed, never stored
    };
    const auto sw = g.register_vertex(path_t("/dev/relay0"), role_t::HANDLER, std::move(relay));

    const auto before = g.read(sw);
    check(ok, before && before->get()->only().bytes().size() == 3,
          "on_read supplies a value the graph never stored");

    (void)g.write(sw, value_of("toggle"));
    check(ok, commands == 1, "a write to a HANDLER vertex runs on_write");

    const auto after = g.read(sw);
    check(ok, after && after->get()->only().bytes().size() == 2,
          "the next read reflects the device, not a store");
    return ok ? 0 : 1;
}
