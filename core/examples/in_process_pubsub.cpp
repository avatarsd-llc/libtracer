/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief In-process publish/subscribe over the L4 graph — the M3 P0 node, end to end.
 *
 * No transport, no wire bytes leave the process. A publisher writes
 * `/sensor/temp`; three subscribers receive the value three different ways:
 *   1. a direct in-process callback  — `subscribe(src, callback)` sugar;
 *   2. a spec-faithful target vertex — `subscribe(src, target)` → a handler sink;
 *   3. a thread blocking in `await()` — the single-shot primitive.
 * Delivery to (1) and (2) is a refcount-bump clone of the same rope value — no
 * byte copy.
 *
 * Runs under ctest as `example_in_process_pubsub`: it @ref check "checks" every
 * delivered value and returns non-zero on any mismatch, so the smoke test guards
 * behavior (each subscriber sees the write) rather than merely exiting cleanly.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::path_t;
using tr::graph::role_t;

/** @brief A 4-byte little-endian VALUE view over @p v (one heap segment). */
tr::view::view_t value_u32(std::uint32_t v) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Decode a little-endian u32 from @p view's first (≤4) bytes. */
std::uint32_t as_u32(const tr::view::view_t& view) {
    const auto b = view.bytes();
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < b.size() && i < 4; ++i)
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[i])) << (8 * i);
    return v;
}

/** @brief Record a failed expectation on @p ok and report it; leaves @p ok false on failure. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    constexpr std::uint32_t kSent = 23;
    tr::graph::graph_t g;

    const tr::graph::vertex_handle_t temp =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // Values captured from each delivery path, verified at the end.
    std::uint32_t cb_got = 0;     // subscriber 1 (callback)
    std::uint32_t sink_got = 0;   // subscriber 2 (target vertex handler)
    std::uint32_t await_got = 0;  // subscriber 3 (await)

    // A "sink" vertex backed by a callback handler — the target of a spec-faithful
    // SUBSCRIBER (subscriber 2 below re-dispatches to it).
    tr::graph::handlers_t sink;
    sink.on_write = [&sink_got](const tr::view::rope_t& in) -> tr::graph::result_t<void> {
        sink_got = as_u32(in.only());
        std::printf("  [sink vertex /log/temp] received %u\n", sink_got);
        return {};
    };
    (void)g.register_vertex(path_t("/log/temp"), role_t::HANDLER, std::move(sink));

    // subscriber_t 1 — direct in-process callback.
    (void)g.subscribe(path_t("/sensor/temp"), [&cb_got](const tr::view::rope_t& v) {
        cb_got = as_u32(v.only());
        std::printf("  [callback sub] received %u\n", cb_got);
    });
    // subscriber_t 2 — spec-faithful target-path subscription -> /log/temp.
    (void)g.subscribe(path_t("/sensor/temp"), path_t("/log/temp"));

    // subscriber_t 3 — a thread blocking in await().
    std::thread waiter([&] {
        auto r = g.await(temp, 2s);
        if (r) {
            await_got = as_u32(r->only());
            std::printf("  [await sub] received %u\n", await_got);
        }
    });
    std::this_thread::sleep_for(50ms);  // let the waiter park in await()

    std::printf("publisher: write /sensor/temp = %u\n", kSent);
    (void)g.write(temp, value_u32(kSent));
    waiter.join();  // joins-after-write: every delivery is complete + visible here

    // Read back the last-known-value (a clone — keeps the segment alive for us).
    auto rb = g.read(temp);
    const std::uint32_t rb_got = rb ? as_u32(rb->only()) : 0u;
    std::printf("read-back /sensor/temp = %u\n", rb_got);

    // Field-write a QoS setting, then discover it via :schema.
    (void)g.write(path_t("/sensor/temp:settings.deadline_ns"), value_u32(5000));
    auto schema = g.read(path_t("/sensor/temp:schema"));
    std::size_t schema_children = 0;
    if (schema) {
        if (auto point = tr::wire::view_as_tlv(schema->only()))
            schema_children = point->children.size();
    }
    std::printf(":schema is a POINT with %zu children\n", schema_children);

    // Guard the semantics, not just a clean exit: every path must see the write.
    bool ok = true;
    check(ok, cb_got == kSent, "callback subscriber received the written value");
    check(ok, sink_got == kSent, "target-vertex subscriber received the written value");
    check(ok, await_got == kSent, "await subscriber received the written value");
    check(ok, rb_got == kSent, "read-back returns the last-known-value");
    check(ok, schema_children == 2, ":schema resolves to a 2-child POINT");
    return ok ? 0 : 1;
}
