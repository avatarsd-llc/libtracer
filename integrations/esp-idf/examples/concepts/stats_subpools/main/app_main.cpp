/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — where a running node's RAM goes: one READ of `:stats.mem.values`,
 *        `.tables` or `.net` answers that sub-pool's census.
 *
 * The default root is one arena, and the graph derives three sub-pools from it: values (what
 * vertices store), tables (registrations and the graph's own containers) and net (the router's
 * and the links' defaults). Each is a `:stats` seam on any vertex (RFC-0010 Amendment 3): a
 * READ answers one `SETTINGS` block of `NAME` / u64 pairs, read here in place with
 * `tlv_node_t`, which allocates nothing. The app stores a few values of different sizes and
 * prints all three blocks: `in_use` against `peak` is the number to size the arena by.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>

#include "libtracer/byteorder.hpp"
#include "libtracer/tracer.hpp"
#include "sdkconfig.h"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::type_t;

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

/** @brief The three counters this example checks, from one sub-pool's census. */
struct census_t {
    std::uint64_t in_use = 0;  /**< @brief Bytes handed out and not returned. */
    std::uint64_t peak = 0;    /**< @brief The high-water mark of `in_use`. */
    std::uint64_t refused = 0; /**< @brief Requests answered by a refusal. */
};

/** @brief Read the census at @p seam and print every counter in it. */
std::optional<census_t> census(const tr::graph::graph_t& g, std::string_view seam) {
    const auto read = g.read(*path_t::parse(seam));
    if (!read) return std::nullopt;
    const tr::view::view_t flat = (*read)->flatten();
    const auto block = tr::wire::tlv_node_t::over(flat.bytes(), tr::mem::null_source());
    if (!block || block->type() != type_t::SETTINGS) return std::nullopt;

    std::printf("%.*s:", static_cast<int>(seam.size()), seam.data());
    census_t c;
    std::string_view name;
    for (const tr::wire::tlv_node_t child : block->children()) {
        const auto body = child.payload();
        if (child.type() == type_t::NAME) {
            name = {reinterpret_cast<const char*>(body.data()), body.size()};
        } else if (child.type() == type_t::VALUE && body.size() == 8) {
            const auto v = tr::detail::load_le<std::uint64_t>(body);
            std::printf(" %.*s=%llu", static_cast<int>(name.size()), name.data(),
                        static_cast<unsigned long long>(v));
            if (name == "in_use") c.in_use = v;
            if (name == "peak") c.peak = v;
            if (name == "refused") c.refused = v;
        }
    }
    std::printf("\n");
    return c;
}

}  // namespace

extern "C" void app_main(void) {
    tr::graph::graph_t g;
    const auto log = g.register_vertex(path_t("/log"), role_t::STORED_VALUE);
    static constexpr std::byte kLine[700]{};
    for (std::size_t n : {16U, 200U, 700U}) {
        const auto value = tr::view::over_bytes(std::span(kLine, n), g.value_backend());
        check(value && g.write(log, *value).has_value(), "write a value to /log");
    }

    const auto values = census(g, "/log:stats.mem.values");
    const auto tables = census(g, "/log:stats.mem.tables");
    const auto net = census(g, "/log:stats.mem.net");

    check(values && values->in_use > 0, "the value sub-pool holds what /log stores");
    check(values && values->peak > values->in_use,
          "its peak still counts the two values the last write replaced");
    check(tables && tables->in_use > 0, "the table sub-pool holds the registration");
    check(net && net->refused == 0, "the net sub-pool answers too, and has refused nothing");
    finish();
}
