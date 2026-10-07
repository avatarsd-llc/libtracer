/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — creation is refused by default, and a parent opts in with app logic.
 *
 * A data write to an address that does not resolve answers `NOT_FOUND` and creates nothing,
 * locally and from a peer alike (RFC-0030 §7.1). An application that wants "a write creates"
 * below one parent installs a CREATION HOOK there (`graph_t::set_creation_hook`, RFC-0030
 * §7.2): the hook is shown the missing child's key and the payload, and registers the child
 * itself, typed as it decides, or refuses.
 *
 * Whether a vertex can carry a hook at all is the compile-time policy `config_t::kCreationHooks`,
 * off by default. In a build that leaves it off, the install is refused with `SCHEMA_NOT_FOUND`
 * and every miss stays refused; the example checks that and stops there.
 *
 * Runs under ctest as `example_graph_creation_hook`; returns non-zero on any failed check.
 */

#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::result_t;
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

/** @brief The app's creation logic: every missing child of its parent becomes a stored value. */
result_t<void> create_stored(void* ctx, tr::graph::vertex_handle_t /*parent*/,
                             std::span<const std::byte> child_key, std::string_view /*subject*/,
                             const tr::view::rope_t& /*payload*/) {
    auto& g = *static_cast<tr::graph::graph_t*>(ctx);
    const auto made = g.register_vertex_key(
        std::vector<std::byte>(child_key.begin(), child_key.end()), role_t::STORED_VALUE);
    if (!made) return std::unexpected(made.error());
    return {};
}

}  // namespace

int main() {
    tr::graph::graph_t g;
    bool ok = true;

    // Nothing is registered, and nothing is created by writing.
    const auto w = g.write(path_t("/zone/soil"), value_of("moist"));
    check(ok, !w && w.error() == status_t::NOT_FOUND,
          "a data write to an unresolved path is NOT_FOUND");
    check(ok, !g.find(path_t("/zone").key()), "and it created no intermediate level");

    // Field writes never create either — there is no vertex to control.
    const auto fw = g.write(path_t("/zone/b:acl"), value_of("x"));
    check(ok, !fw && fw.error() == status_t::NOT_FOUND,
          "a :field write to a nonexistent vertex is NOT_FOUND");

    // Opt /zone in. A build without creation hooks refuses the install by value.
    const auto zone = g.register_vertex(path_t("/zone"), role_t::STORED_VALUE);
    const auto installed = g.set_creation_hook(zone, {&create_stored, &g});
    if (!tr::graph::kCreationHooks) {
        check(ok, !installed && installed.error() == status_t::SCHEMA_NOT_FOUND,
              "this build has no hook slot, so the install is refused");
        return ok ? 0 : 1;
    }
    check(ok, installed.has_value(), "the app installs a creation hook on /zone");
    check(ok, g.write(path_t("/zone/soil"), value_of("moist")).has_value(),
          "a write to the missing /zone/soil now creates it, through the hook");
    check(ok, g.read(path_t("/zone/soil")).has_value(),
          "the created vertex serves the value that created it");

    // The hook decides one level. It creates /zone/a, which carries no hook of its own, so the
    // next level, /zone/a/b, is refused: `mkdir -p` holds only where every level opted in.
    const auto deep = g.write(path_t("/zone/a/b"), value_of("x"));
    check(ok, !deep && deep.error() == status_t::NOT_FOUND,
          "a deeper miss is decided by the new child's own hook, and it has none");
    return ok ? 0 : 1;
}
