/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The delivery policy is PER SUBSCRIPTION — `durability_request` replays the
 *        producer's latched last value on join (RFC-0022 §3.A).
 *
 * `delivery_policy_t` is the packed 16-bit field a wire subscriber carries in its
 * `SUBSCRIBER.SETTINGS` child; the host sugar takes the same bits. Bit 5,
 * `kDurabilityRequest`, is the one bit the reference implementation consumes today: the
 * requesting subscriber gets one delivery at subscribe time carrying the producer's
 * last-known-value. Two subscribers on the SAME producer differ, which is what makes the
 * policy per-subscription rather than per-vertex.
 *
 * Runs under ctest as `example_sub_durability_latch`; it self-checks and returns non-zero
 * on any mismatch.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::delivery_policy_t;
using tr::graph::path_t;
using tr::graph::role_t;

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
    (void)g.write(src, value_byte(0x5A));  // a last-known-value EXISTS before either join

    int durable_seen = 0, plain_seen = 0;
    std::uint8_t latched = 0;
    auto on_durable = [&](const tr::view::rope_t& v) {
        ++durable_seen;
        latched = std::to_integer<std::uint8_t>(v.only().bytes()[0]);
    };
    auto on_plain = [&](const tr::view::rope_t&) { ++plain_seen; };

    (void)g.subscribe(path_t("/sensor/temp"), on_durable,
                      delivery_policy_t{delivery_policy_t::kDurabilityRequest});
    (void)g.subscribe(path_t("/sensor/temp"), on_plain);  // all-zero policy = today's default
    const int join_durable = durable_seen, join_plain = plain_seen;
    const std::uint8_t join_latched = latched;
    std::printf("on join: durable=%d (0x%02x), plain=%d\n", join_durable, join_latched, join_plain);

    (void)g.write(src, value_byte(0x77));
    std::printf("after one write: durable=%d, plain=%d\n", durable_seen, plain_seen);

    bool ok = true;
    check(ok, join_durable == 1 && join_latched == 0x5A,
          "the request latched the current value on join");
    check(ok, join_plain == 0, "the same producer delivered nothing to the non-requesting edge");
    check(ok, durable_seen == 2 && plain_seen == 1, "both edges take the later write");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
