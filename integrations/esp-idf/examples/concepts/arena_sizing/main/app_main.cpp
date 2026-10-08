/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the arena's size is a compile-time number from `config_t`, sized against
 *        the node's peak, and a node that outgrows it is told so: by value at run time, by a
 *        sizing message at init.
 *
 * `CONFIG_LIBTRACER_ARENA_BYTES` binds `tr::graph::config_t::kArenaBytes`, the size of the one
 * `.bss` array behind the default root (ADR-0083 Decision 4). This app sets it to 8 KiB in
 * `sdkconfig.defaults`, registers and writes the vertices it is built to hold, and reads the
 * root's census to see the headroom left. Then it keeps registering with `try_register_vertex`,
 * the failable form, until the arena refuses: a run-time registration answers an error and the
 * node keeps running. `register_vertex`, the infallible init form (ADR-0056), cannot answer one;
 * past the arena it prints the sizing message and aborts. `CONFIG_EXAMPLE_SHOW_INIT_EXHAUSTION`
 * runs that last step too; it is off by default because on a board the abort is a reset.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "libtracer/tracer.hpp"
#include "sdkconfig.h"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief The vertices this node is built to hold. */
constexpr int kSensors = 8;

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

/** @brief The path `/s/<i>`, parsed. */
path_t sensor(int i) {
    char text[16];
    std::snprintf(text, sizeof text, "/s/%d", i);
    return *path_t::parse(text);
}

}  // namespace

extern "C" void app_main(void) {
    tr::mem::block_source_t& root = tr::mem::default_root();
    std::printf("config_t::kArenaBytes = %zu (CONFIG_LIBTRACER_ARENA_BYTES)\n",
                tr::mem::kArenaBytes);

    tr::graph::graph_t g;
    for (int i = 0; i < kSensors; ++i) {
        const auto v = g.register_vertex(sensor(i), role_t::STORED_VALUE);
        const std::uint32_t reading = 200 + i;
        const auto value =
            tr::view::over_bytes(std::as_bytes(std::span(&reading, 1)), g.value_backend());
        check(value && g.write(v, *value).has_value(), "register and write one sensor");
    }
    const tr::mem::source_stats_t s = root.stats();
    std::printf("%d sensors: %zu of %zu bytes carved, %zu bytes of headroom\n", kSensors, s.in_use,
                s.capacity, s.capacity - s.in_use);
    check(s.capacity == tr::mem::kArenaBytes && s.in_use < s.capacity, "the node fits");

    // Past the arena at run time: the failable form answers an error, and nothing aborts.
    int extra = 0;
    while (extra < 10000 && g.try_register_vertex(sensor(kSensors + extra), role_t::STORED_VALUE))
        ++extra;
    const tr::mem::source_stats_t after = root.stats();
    std::printf("try_register_vertex refused after %d more; %zu refusals, largest %zu bytes\n",
                extra, after.refused, after.largest_refused);
    check(extra < 10000 && after.refused > 0, "the arena refused by value, and the node runs on");

#if CONFIG_EXAMPLE_SHOW_INIT_EXHAUSTION
    // Past the arena at init: register_vertex is infallible, so this prints the sizing
    // message naming the sub-pool and the bytes it needed, and aborts.
    (void)g.register_vertex(path_t("/one/too/many"), role_t::STORED_VALUE);
#endif
    finish();
}
