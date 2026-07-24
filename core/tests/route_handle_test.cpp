/**
 * @file
 * @brief route_handle_t unit test (Brick 4, ADR-0038 §3 / ADR-0039): the label state is per-
 *        connection — pmr-backed tables with a per-link mutex.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Covers: bind/lookup,
 * rebind-replaces, stale lookup, per-link label-space isolation, ensure_egress
 * reuse vs fresh, egress_route retention (NACK re-advertise), clear_link resets
 * one link only (allocator included), counts, and a whole-lifecycle run inside a
 * slab-backed monotonic resource with a null upstream (zero global heap for the
 * table state — the ADR-0039 host-owned-memory claim). #488: clear_link reclaims
 * the per-link shell (churn of N distinct names returns link_count() to steady
 * state) while staying UAF-free under a concurrent writer x clear_link race (the
 * shared_ptr pin — a TSan gate, run instrumented by the core-ci tsan job).
 */

#include "libtracer/route_handle.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace tr::net;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> route_bytes(std::uint8_t tag) {
    return {std::byte{0x06}, std::byte{0x40}, std::byte{0x01}, std::byte{0x00}, std::byte{tag}};
}

void exercise(route_handle_t& h) {
    // Per-link label spaces are independent and start at 1.
    check(h.alloc_label("a") == 1 && h.alloc_label("a") == 2 && h.alloc_label("b") == 1,
          "labels are per-link monotonic from 1");

    // Ingress: bind, lookup, rebind replaces, unknown label is stale.
    h.bind_ingress(
        "a", 7,
        handle_binding_t{
            .terminus = true, .down_link = {}, .out_label = 0, .local_route = route_bytes(1)});
    auto b = h.lookup_ingress("a", 7);
    check(b && b->terminus && b->local_route == route_bytes(1), "ingress bind + lookup");
    h.bind_ingress(
        "a", 7,
        handle_binding_t{.terminus = false, .down_link = "b", .out_label = 9, .local_route = {}});
    b = h.lookup_ingress("a", 7);
    check(b && !b->terminus && b->down_link == "b" && b->out_label == 9,
          "rebinding a label replaces the binding");
    check(!h.lookup_ingress("a", 8) && !h.lookup_ingress("zz", 7),
          "unknown label / unknown link ⇒ stale (nullopt)");

    // Egress: record + retrieve (the NACK re-advertise path).
    h.record_egress("b", 3, route_bytes(2));
    const auto r = h.egress_route("b", 3);
    check(r && *r == route_bytes(2), "egress route retained for re-advertise");

    // ensure_egress: fresh once per (link, route), then reused; distinct per link.
    const auto [l1, fresh1] = h.ensure_egress("b", route_bytes(4));
    const auto [l2, fresh2] = h.ensure_egress("b", route_bytes(4));
    const auto [l3, fresh3] = h.ensure_egress("c", route_bytes(4));
    check(fresh1 && !fresh2 && l1 == l2, "ensure_egress mints once then reuses");
    check(fresh3 && l3 == 1, "the same route on another link is a separate flow");

    check(h.ingress_count() == 1 && h.egress_count() == 3, "counts see all links");

    // clear_link drops ONE link's state — bindings, egress, and the allocator.
    h.clear_link("a");
    check(!h.lookup_ingress("a", 7), "cleared link's binding is stale");
    check(h.alloc_label("a") == 1, "cleared link's allocator restarts at 1");
    check(h.egress_route("b", 3).has_value(), "other links untouched by clear_link");
    check(h.ingress_count() == 0 && h.egress_count() == 3, "counts after clear");
}

/**
 * @brief #488 — clear_link RECLAIMS a departed link's shell (was insert-only), and the
 *        erase-while-referenced path is use-after-free-free (the shared_ptr pin).
 *
 * Runs on the default (freeing) resource so a reclaimed shell is truly returned. The
 * concurrent section is a TSan gate: writers hammer a small shared name set while a
 * clearer erases the same names; the pin must keep every handed-out table alive.
 */
void churn_and_race() {
    std::printf(" #488 churn + erase-while-referenced:\n");
    route_handle_t h;
    check(h.link_count() == 0, "a fresh route_handle holds no link shells");

    // Reclamation: N distinct link names each mint one shell; clearing each reclaims it,
    // so link_count() returns to its steady state (it was insert-only before #488).
    constexpr int kNames = 64;
    for (int i = 0; i < kNames; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "peer%d", i);
        (void)h.ensure_egress(name, route_bytes(static_cast<std::uint8_t>(i)));
    }
    check(h.link_count() == kNames, "each distinct link name creates exactly one shell");
    for (int i = 0; i < kNames; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "peer%d", i);
        h.clear_link(name);
    }
    check(h.link_count() == 0, "clear_link reclaims the shell — links_ back to steady state");

    // Erase-while-referenced: writers churn ~2000 labels across 6 shared names while a
    // clearer erases those same names. A handed-out shared_ptr must pin its table so an
    // erase never frees a table mid-write (TSan/ASan would flag a UAF or race here).
    constexpr int kThreads = 4;
    constexpr int kIters = 1500;
    std::atomic<bool> go{false};
    auto writer = [&](int seed) {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "L%d", (seed + i) % 6);
            (void)h.ensure_egress(name, route_bytes(static_cast<std::uint8_t>(i)));
            h.bind_ingress(name, static_cast<std::uint16_t>((i & 0x7FFF) + 1),
                           handle_binding_t{.terminus = true});
            (void)h.lookup_ingress(name, static_cast<std::uint16_t>((i & 0x7FFF) + 1));
        }
    };
    auto clearer = [&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < kIters; ++i) {
            char name[8];
            std::snprintf(name, sizeof name, "L%d", i % 6);
            h.clear_link(name);
        }
    };
    std::vector<std::thread> ts;
    ts.reserve(kThreads + 1);
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(writer, t);
    ts.emplace_back(clearer);
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    // No crash / no sanitizer report IS the assertion; the registry stays bounded to the
    // live name set (<= 6), never the ~2000 distinct labels churned through it.
    check(h.link_count() <= 6, "concurrent churn + clear keeps shells bounded to live names");
}

}  // namespace

int main() {
    std::printf("route_handle_t (Brick 4 — per-connection pmr label tables):\n");

    {
        std::printf(" default resource:\n");
        route_handle_t h;
        exercise(h);
    }
    {
        // The ADR-0039 claim: the whole label lifecycle draws from the host slab —
        // a null upstream would throw on ANY global-heap fallback for table state.
        std::printf(" slab resource (null upstream — zero global heap):\n");
        alignas(std::max_align_t) static std::array<std::byte, 16384> slab;
        std::pmr::monotonic_buffer_resource mr(slab.data(), slab.size(),
                                               std::pmr::null_memory_resource());
        route_handle_t h(&mr);
        exercise(h);
    }

    churn_and_race();

    if (g_failures == 0) {
        std::printf("route_handle: ALL PASS\n");
        return 0;
    }
    std::printf("route_handle: %d FAILURE(S)\n", g_failures);
    return 1;
}
