/**
 * @file
 * @brief `graph_t::set_policy` refuses a vertex that is no longer registered, so the next
 *        registration at the same path inherits nothing (#2669).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A retired vertex stays allocated as a placeholder, and the next registration at its path
 * fills that same object in place, naming every policy member the caller leaves unset with
 * what the placeholder holds. A policy installed through a handle held across the retire would
 * therefore reach the next occupant. `set_policy` checks registration under the map lock, the
 * hold registration and retirement take, and answers `NOT_FOUND` like the other owner-side
 * verbs (`set_creation_hook`, `hide_from_enumeration`).
 *
 * - **Stale handle.** Retire, `set_policy` through the old handle (refused, `NOT_FOUND`),
 *   re-register the path with no policy: every policy member and the app-field table are at
 *   the values a never-touched vertex holds.
 * - **Concurrent.** One thread calls `set_policy` through the handle the whole time while the
 *   main thread retires and revives the path; between each retire and revive the placeholder
 *   must hold the never-touched values. Under TSan this is also the data-race check.
 */

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::app_access_t;
using tr::graph::app_field_t;
using tr::graph::delivery_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::retention_t;
using tr::graph::role_t;
using tr::graph::share_hint_t;
using tr::graph::status_t;
using tr::graph::vertex_handle_t;
using tr::graph::vertex_policy_t;
using tr::graph::vertex_t;
using tr::testing::check;

/** @brief The vertex behind a handle, on the `qos_policy_test` precedent. */
[[nodiscard]] const vertex_t* vx(const vertex_handle_t& v) { return std::bit_cast<vertex_t*>(v); }

/** @brief Every policy member a vertex holds, as `set_policy` can move it. */
struct policy_state_t {
    retention_t retention;               /**< @brief Retention. */
    std::uint32_t depth;                 /**< @brief Ring depth. */
    share_hint_t share_hint;             /**< @brief Copy-or-share hint. */
    const tr::mem::block_source_t* ring; /**< @brief Ring source. */
    bool ring_reliable;                  /**< @brief Ring reliability. */
    delivery_mode_t mode;                /**< @brief Delivery mode. */
    std::size_t app_fields;              /**< @brief Installed app-field slots. */
    bool operator==(const policy_state_t&) const = default; /**< @brief Member-wise. */
};

/** @brief @p s with its retention taken from @p like — a placeholder's retention is its
 *         `STORED_VALUE` role's, not the `STREAM` reference's. */
[[nodiscard]] policy_state_t retention_of(policy_state_t s, const policy_state_t& like) {
    s.retention = like.retention;
    return s;
}

/** @brief Read @p v's policy members. */
[[nodiscard]] policy_state_t state_of(const vertex_handle_t& v) {
    const vertex_t* const p = vx(v);
    return {.retention = p->retention(),
            .depth = p->retention_depth(),
            .share_hint = p->share_hint(),
            .ring = p->ring_source(),
            .ring_reliable = p->ring_reliable(),
            .mode = p->delivery_mode(),
            .app_fields = p->app_field_slots().size()};
}

/**
 * @brief A policy naming every member away from its default. The stale calls leave retention
 *        unset: a retired vertex is a `STORED_VALUE` placeholder, which a `STREAM`'s `N` would
 *        make illegal, and an illegal policy is refused before the registration matters.
 */
[[nodiscard]] vertex_policy_t everything(tr::mem::block_source_t& ring, bool name_retention) {
    std::vector<app_field_t> table;
    table.push_back(app_field_t{.name = "kp", .access = app_access_t::RW});
    return {.retention = name_retention ? std::optional(retention_t::N) : std::nullopt,
            .depth = 16,
            .share_hint = share_hint_t::COPY,
            .ring_source = &ring,
            .ring_reliable = true,
            .delivery_mode = delivery_mode_t::UNCONDITIONAL,
            .app_fields = std::move(table)};
}

/** @brief A stale handle is refused, and the re-registered path inherits nothing. */
void stale_handle_refused() {
    std::printf("set_policy through a retired vertex's handle is refused:\n");
    graph_t g;
    const policy_state_t fresh = state_of(g.register_vertex(path_t("/ref"), role_t::STREAM));
    const path_t at("/s");
    const vertex_handle_t h = g.register_vertex(at, role_t::STREAM);
    check(g.set_policy(h, everything(tr::mem::heap_source(), true)).has_value(),
          "set_policy on the live vertex succeeds");
    check(g.retire(h).has_value(), "/s retired");
    check(state_of(h) == retention_of(fresh, state_of(h)),
          "the retired placeholder holds the never-touched values");
    const tr::graph::result_t<void> r = g.set_policy(h, everything(tr::mem::heap_source(), false));
    check(!r.has_value() && r.error() == status_t::NOT_FOUND,
          "set_policy through the stale handle answers NOT_FOUND");
    const vertex_handle_t again = g.register_vertex(at, role_t::STREAM);
    check(state_of(again) == fresh,
          "the re-registered vertex inherits nothing: policy members and app fields at defaults");
}

/** @brief A concurrent set_policy never leaves state on a retired placeholder. */
void concurrent_retire_and_set_policy() {
    std::printf("set_policy racing retire/revive leaves nothing on the placeholder:\n");
    graph_t g;
    const policy_state_t fresh = state_of(g.register_vertex(path_t("/ref"), role_t::STREAM));
    const path_t at("/s");
    const vertex_handle_t h = g.register_vertex(at, role_t::STREAM);
    std::atomic<bool> stop{false};
    std::thread setter([&] {
        while (!stop.load(std::memory_order_relaxed))
            (void)g.set_policy(h, everything(tr::mem::heap_source(), false));
    });
    constexpr int kRounds = 2000;
    int clean = 0;
    for (int i = 0; i < kRounds; ++i) {
        (void)g.retire(h);
        const policy_state_t now = state_of(h);
        clean += now == retention_of(fresh, now) ? 1 : 0;
        (void)g.register_vertex(at, role_t::STREAM);
    }
    stop.store(true, std::memory_order_relaxed);
    setter.join();
    check(clean == kRounds, "every retired placeholder held the never-touched values");
}

}  // namespace

int main() {
    stale_handle_refused();
    concurrent_retire_and_set_policy();
    return tr::testing::summary("retired_policy");
}
