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
 * `reclaim_strict_t` forbids this shape outright — it debug-asserts on a re-entrant
 * unsubscribe — so THIS EXAMPLE APPLIES TO `reclaim_local_t` ONLY. Both policies live in
 * `libtracer/reclaim.hpp`, and the binding is a build-time type, so the example follows it
 * with `if constexpr` and SKIPS its body where the policy forbids the shape (the same way
 * `core/tests/reclaim_test.cpp` drops its re-entrant cases). A policy the example does not
 * apply to must not break the build — every example compiles under every binding.
 *
 * Runs under ctest as `example_sub_unsubscribe_from_dispatch`; it self-checks and returns
 * non-zero on any mismatch. Under `reclaim_strict_t` it prints a skip line and passes.
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

/**
 * @brief The example proper — retire a subscription from inside its own delivery.
 *
 * Written unguarded, because under `reclaim_local_t` every line of it is valid; the ONE
 * policy branch lives in `main`, so what a reader studies here is the pattern itself.
 *
 * @return True when every expectation held.
 */
bool run_reentrant_unsubscribe() {
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
    return ok;
}

}  // namespace

int main() {
    const auto policy = tr::graph::reclaim_policy_t::kName;
    std::printf("reclamation policy bound by this build: %.*s\n", static_cast<int>(policy.size()),
                policy.data());

    bool ok = true;
    if constexpr (tr::graph::reclaim_policy_t::kReentrantUnsubscribe) {
        ok = run_reentrant_unsubscribe();
    } else {
        // `reclaim_strict_t` FORBIDS the shape this whole example is about, and debug-asserts
        // on it — running the body would be asserting that an abort happens. The example is
        // therefore skipped, not failed, and certainly not made to break the build: it simply
        // does not apply to this binding. Read it against the default policy.
        std::printf("skipped: this build forbids unsubscribing from inside a delivery\n");
    }
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
