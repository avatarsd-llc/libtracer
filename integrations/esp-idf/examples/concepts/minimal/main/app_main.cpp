/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — the component and nothing else: `REQUIRES libtracer`, a `graph_t` built
 *        with no argument, and every byte it holds carved from one static arena.
 *
 * On an ESP-IDF build the component binds `kSlabPool = false`, so the default root
 * (`tr::mem::default_root()`) is a static arena of `CONFIG_LIBTRACER_ARENA_BYTES` in `.bss`
 * (ADR-0083 Decision 4). A graph built without a source draws its tables, and the values it
 * stores, from that arena. This app registers one vertex, writes it, reads it back, and prints
 * what the arena carved for it. The transports are switched off in `sdkconfig.defaults`: an
 * in-process node needs none of them.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "libtracer/tracer.hpp"
#include "sdkconfig.h"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

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
    tr::mem::block_source_t& root = tr::mem::default_root();
    const std::size_t before = root.stats().in_use;

    tr::graph::graph_t g;  // no source: the default root, the static arena
    const auto temp = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    static constexpr char kText[] = "21.5C";
    const auto bytes = std::as_bytes(std::span(kText, sizeof kText - 1));
    const auto value = tr::view::over_bytes(bytes, g.value_backend());
    check(value.has_value(), "the value's bytes came from the graph's value backend");
    check(value && g.write(temp, *value).has_value(), "write /sensor/temp");

    const auto read = g.read(temp);
    const auto got = read ? (*read)->only().bytes() : std::span<const std::byte>{};
    check(got.size() == bytes.size() && std::memcmp(got.data(), bytes.data(), got.size()) == 0,
          "read /sensor/temp serves what was written");

    const tr::mem::source_stats_t s = root.stats();
    std::printf("default root \"%s\": %zu of %zu arena bytes carved, %zu of them by this node\n",
                root.name(), s.in_use, s.capacity, s.in_use - before);
    check(s.capacity == tr::mem::kArenaBytes, "the root is the arena config_t sized");
    check(s.in_use > before, "the graph's tables and its value were carved from it");
    finish();
}
