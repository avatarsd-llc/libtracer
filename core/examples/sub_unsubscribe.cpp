/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Unsubscribe, and be TOLD when the subscriber's context is dead (ADR-0080).
 *
 * `unsubscribe(sub)` retires the edge: the next fan-out snapshot skips the slot. What it
 * cannot say on its own is when the `{fn, ctx}` pair stopped being reachable, so the
 * two-argument overload takes a @ref tr::graph::subscriber_release_fn_t and the LIBRARY
 * signals the caller — no polling, no waiting. Called from OUTSIDE a delivery (the case
 * here, and the ordinary one) every shipped policy runs the hook inline, before
 * `unsubscribe()` returns, so the caller may free its context on that return.
 *
 * Runs under ctest as `example_sub_unsubscribe`; it self-checks and returns non-zero on
 * any mismatch.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;

/** @brief The subscriber's own state — the `ctx` the edge carries back on every delivery. */
struct sink_t {
    int seen = 0;          /**< @brief Deliveries observed. */
    bool released = false; /**< @brief Set by the release hook, exactly once. */
};

/** @brief The per-delivery sink (`subscriber_fn_t`): a plain function pointer, no erasure. */
void on_delivery(void* ctx, const tr::view::rope_t&) { ++static_cast<sink_t*>(ctx)->seen; }

/** @brief The release hook — libtracer calls it once, at the policy's grace point. */
void on_release(void* ctx) { static_cast<sink_t*>(ctx)->released = true; }

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

}  // namespace

int main() {
    tr::graph::graph_t g;
    const tr::graph::vertex_handle_t src =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    sink_t sink;
    const auto sub = g.subscribe(path_t("/sensor/temp"), &on_delivery, &sink);
    (void)g.write(src, value_byte(0x01));

    const auto gone = g.unsubscribe(*sub, &on_release);
    const bool released_on_return = sink.released;
    (void)g.write(src, value_byte(0x02));  // after the retire: reaches nobody

    sink.released = false;  // a second retire owes no second signal
    const auto again = g.unsubscribe(*sub, &on_release);
    std::printf("seen=%d released_on_return=%d, second unsubscribe -> %s (hook ran: %d)\n",
                sink.seen, static_cast<int>(released_on_return),
                again.has_value() ? "ok" : tr::graph::to_string(again.error()),
                static_cast<int>(sink.released));

    bool ok = true;
    check(ok, sink.seen == 1,
          "the write before the unsubscribe was delivered, the one after was not");
    check(ok, gone.has_value() && released_on_return,
          "the hook ran inline, before unsubscribe returned");
    check(ok, !again.has_value() && again.error() == status_t::NOT_FOUND && !sink.released,
          "an already-cleared handle answers NOT_FOUND — and runs no hook");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
