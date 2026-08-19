/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Unsubscribing from INSIDE a delivery — the deferred grace point (ADR-0080).
 *
 * A callback that retires its own subscription cannot be told "the pair is dead" on the
 * spot: the fan-out that called it is still walking a snapshot naming that `{fn, ctx}`
 * pair. Under the default `reclaim_local_t` the retired pair is PARKED and the release
 * hook runs when this thread's dispatch stack unwinds to depth 0 — i.e. before the
 * `write()` that started the delivery returns. The retirement itself is immediate: the
 * next fan-out skips the slot.
 *
 * `reclaim_strict_t` forbids this shape outright, which is why the example prints the
 * policy this build bound. Both live in `libtracer/reclaim.hpp`.
 *
 * Runs under ctest as `example_sub_unsubscribe_from_dispatch`; it self-checks and returns
 * non-zero on any mismatch.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief The subscriber's `ctx`: its own state plus what it needs to retire itself. */
struct sink_t {
    tr::graph::graph_t* g = nullptr; /**< @brief The graph to unsubscribe from. */
    tr::graph::subscription_t sub{}; /**< @brief This subscription's handle. */
    int seen = 0;                    /**< @brief Deliveries observed. */
    bool released = false;           /**< @brief Set by the release hook, exactly once. */
    bool released_inside_cb = false; /**< @brief Was it already set when the callback left? */
};

/** @brief The release hook — libtracer calls it once, at the policy's grace point. */
void on_release(void* ctx) { static_cast<sink_t*>(ctx)->released = true; }

/** @brief A delivery that retires its own subscription (the re-entrant case). */
void on_delivery(void* ctx, const tr::view::rope_t&) {
    auto* s = static_cast<sink_t*>(ctx);
    ++s->seen;
    (void)s->g->unsubscribe(s->sub, &on_release);
    s->released_inside_cb = s->released;  // still parked here — dispatch has not unwound
}

/** @brief A one-byte VALUE view over @p b (one heap segment). */
tr::view::view_t value_byte(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Record a failed expectation on @p ok and report it. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

// A build that bound `reclaim_strict_t` forbids the shape this example is ABOUT, so say so
// at compile time rather than failing the smoke test at run time.
static_assert(tr::graph::reclaim_policy_t::kReentrantUnsubscribe,
              "this example unsubscribes from inside a delivery; reclaim_strict forbids that");

}  // namespace

int main() {
    const auto policy = tr::graph::reclaim_policy_t::kName;
    std::printf("reclamation policy bound by this build: %.*s\n", static_cast<int>(policy.size()),
                policy.data());

    tr::graph::graph_t g;
    const tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    sink_t sink;
    sink.g = &g;
    sink.sub = *g.subscribe(path_t("/sensor/temp"), &on_delivery, &sink);

    (void)g.write(src, value_byte(0x01));  // delivers, and the callback retires itself
    std::printf("after write #1: seen=%d released=%d (inside the callback it was %d)\n", sink.seen,
                static_cast<int>(sink.released), static_cast<int>(sink.released_inside_cb));
    (void)g.write(src, value_byte(0x02));  // the slot is already retired

    bool ok = true;
    check(ok, sink.seen == 1, "the retire took effect at once — the second write reached nobody");
    check(ok, sink.released, "the hook ran before write() returned");
    check(ok, !sink.released_inside_cb,
          "and not one moment earlier — the fan-out was still walking the snapshot");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
