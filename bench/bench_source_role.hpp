/**
 * @file
 * @brief The fixture shared by the two halves of the retention-role write bench (#1505):
 *        `bench_source_role` (timing) and `bench_source_role_alloc` (the heap census).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The two halves live in separate binaries on the `bench_store_sweep` / `bench_store_escape`
 * precedent, and for the same reason: the census owns a global `operator new` override whose
 * per-allocation atomic load would be charged to whichever arm allocates more — which is
 * exactly the retaining arm this bench compares against. A timing binary that carried the
 * override would bias the comparison in favour of the very side under suspicion.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "libtracer/rope.hpp"
#include "libtracer/tracer.hpp"

namespace bench_role {

/** @brief Bytes per link in the written value. */
inline constexpr std::size_t kBytes = 64;

/**
 * @brief Stable per-link buffers, so building the written value allocates nothing.
 *
 * Borrowed views over caller-owned bytes: the timed loop must measure the write path, not a
 * per-iteration segment allocation. `rope_t::kInline` is 2, so a fixture of 1 link stays
 * inline through a clone and one of 4 links spills to the heap — the axis this bench sweeps.
 */
struct value_fixture_t {
    std::vector<std::vector<std::byte>> buffers;

    explicit value_fixture_t(std::size_t links) {
        for (std::size_t i = 0; i < links; ++i) buffers.emplace_back(kBytes, std::byte{0xAB});
    }

    /** @brief A borrowed-view rope of `buffers.size()` links — zero alloc, zero copy. */
    [[nodiscard]] tr::view::rope_t make() const {
        tr::view::rope_t r;
        (void)r.try_reserve(buffers.size());
        for (const std::vector<std::byte>& b : buffers)
            r.append(tr::view::view_t::over(tr::view::borrow_const(std::span<const std::byte>(b))));
        return r;
    }
};

/**
 * @brief Register `/bench/src` with the role under test, wiring a HANDLER's `on_write` to
 *        @p hits so the arm can be proven live before it is timed.
 *
 * @param handler `true` for `HANDLER` (retains nothing — clones to notify), `false` for
 *                `STORED_VALUE` (retains — delivers the pointer `store_value` published).
 */
[[nodiscard]] inline tr::graph::vertex_handle_t register_src(tr::graph::graph_t& g,
                                                             const tr::graph::path_t& src,
                                                             bool handler,
                                                             std::atomic<std::uint64_t>& hits) {
    if (!handler) return g.register_vertex(src, tr::graph::role_t::STORED_VALUE);
    tr::graph::handlers_t h;
    h.on_write = [&hits](const tr::view::rope_t&,
                         const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        hits.fetch_add(1, std::memory_order_relaxed);
        return {};
    };
    return g.register_vertex(src, tr::graph::role_t::HANDLER, std::move(h));
}

}  // namespace bench_role
