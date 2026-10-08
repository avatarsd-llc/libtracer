/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a node that runs on one task compiles its pool's locks away: an arena
 *        root over `tr::no_guard_t`, injected into the `graph_t`.
 *
 * The component's default root (`tr::mem::mcu_root_t`) locks its arena and each sub-pool with
 * the build's `guard_t`, an interrupt-masked critical section on a chip, because a default has
 * to be safe for any number of tasks. A node whose graph is touched by one task only declares
 * its own root with `tr::no_guard_t` as the lock: each lock is then an empty type, every
 * lock and unlock compiles to nothing, and the root is never larger. The lock is a template
 * argument, so the choice costs nothing at run time and cannot be changed by mistake there.
 *
 * The root is the application's, over its own static region, so the linker map shows it as the
 * app's `.bss`. An injected root serves every purpose itself: the graph derives no sub-pools
 * from it. This covers the pool. The graph's own vertex locks and value slot still use the
 * build's `guard_t`; a single-threaded core build binds that to `tr::no_guard_t` in its
 * `libtracer/config_override.hpp`, which this component generates and does not expose.
 */

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <type_traits>

#include "libtracer/guard.hpp"
#include "libtracer/mem_arena.hpp"
#include "libtracer/tracer.hpp"
#include "sdkconfig.h"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief Rows in the build's size-class table. */
constexpr std::size_t kClasses = std::size(tr::graph::config_t::kSizeClasses);

/** @brief The single-task root: the default root's shape with `tr::no_guard_t` as its lock. */
using st_root_t = tr::mem::arena_root_t<tr::no_guard_t, kClasses>;

static_assert(std::is_empty_v<tr::no_guard_t> && sizeof(st_root_t) <= sizeof(tr::mem::mcu_root_t),
              "the lock is an empty type: it holds no state and its calls compile to nothing");

/** @brief The app's arena region and the root's free-list heads, both zero-filled `.bss`. */
alignas(64) constinit std::array<std::byte, 16384> g_region{};
constinit std::array<void*, st_root_t::kHeads> g_heads{};

/** @brief The root, constant-initialized over them. */
constinit st_root_t g_root(
    g_region, std::span<const std::size_t, kClasses>(tr::graph::config_t::kSizeClasses), g_heads);

/** @brief Failed checks so far. */
int g_failures = 0;

/** @brief Print @p what with its verdict and count a failure. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief Print the verdict; on the `linux` target also exit with it, so CI can run this. */
void finish() {
    std::printf("RESULT %s\n", g_failures == 0 ? "ok" : "FAIL");
#if CONFIG_IDF_TARGET_LINUX
    std::exit(g_failures == 0 ? 0 : 1);
#endif
}

}  // namespace

extern "C" void app_main(void) {
    std::printf("root object: %zu B with tr::no_guard_t, %zu B with the build's guard_t\n",
                sizeof(st_root_t), sizeof(tr::mem::mcu_root_t));

    tr::graph::graph_t g(g_root);  // every block this graph draws comes from g_root
    const auto count = g.register_vertex(path_t("/counter"), role_t::STORED_VALUE);
    for (std::uint32_t i = 1; i <= 100; ++i) {
        const auto value = tr::view::over_bytes(std::as_bytes(std::span(&i, 1)), g.value_backend());
        if (!value || !g.write(count, *value)) check(false, "write /counter");
    }
    const auto read = g.read(count);
    check(read && (*read)->only().bytes().size() == sizeof(std::uint32_t),
          "a hundred writes from one task, and the last one reads back");
    check(!g.derives_sub_pools(), "an injected root serves every purpose itself");

    const tr::mem::source_stats_t s = g_root.stats();
    std::printf("app root: %zu of %zu bytes carved, %zu refusals\n", s.in_use, s.capacity,
                s.refused);
    check(s.in_use > 0 && s.refused == 0, "the graph drew from the app's root alone");
    check(tr::mem::default_root().stats().in_use == 0, "and nothing from the default arena");
    finish();
}
