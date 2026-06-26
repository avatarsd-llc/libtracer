// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// In-process publish/subscribe over the L4 graph — the M3 P0 node, end to end,
// with no transport and no wire bytes leaving the process. A publisher writes
// /sensor/temp; three subscribers receive the value three different ways:
//   1. a direct in-process callback     (subscribe(src, callback) sugar)
//   2. a spec-faithful target vertex     (subscribe(src, target) -> a handler sink)
//   3. a thread blocking in await()      (the single-shot primitive)
// Delivery to (1) and (2) is a refcount-bump clone of the same view_t — no byte copy.

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

tr::view::view_t value_u32(std::uint32_t v) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}

std::uint32_t as_u32(const tr::view::view_t& view) {
    const auto b = view.bytes();
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < b.size() && i < 4; ++i)
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[i])) << (8 * i);
    return v;
}

}  // namespace

int main() {
    tr::graph::graph_t g;

    tr::graph::vertex_t* temp =
        *g.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);

    // A "sink" vertex backed by a callback handler — the target of a spec-faithful
    // SUBSCRIBER (subscriber 2 below re-dispatches to it).
    tr::graph::handlers_t sink;
    sink.on_write = [](const tr::view::view_t& in) -> tr::graph::result_t<void> {
        std::printf("  [sink vertex /log/temp] received %u\n", as_u32(in));
        return {};
    };
    (void)g.register_vertex(*path_t::parse("/log/temp"), role_t::HANDLER, std::move(sink));

    // subscriber_t 1 — direct in-process callback.
    (void)g.subscribe(*path_t::parse("/sensor/temp"), [](const tr::view::view_t& v) {
        std::printf("  [callback sub] received %u\n", as_u32(v));
    });
    // subscriber_t 2 — spec-faithful target-path subscription -> /log/temp.
    (void)g.subscribe(*path_t::parse("/sensor/temp"), *path_t::parse("/log/temp"));

    // subscriber_t 3 — a thread blocking in await().
    std::thread waiter([&] {
        auto r = g.await(temp, 2s);
        if (r) std::printf("  [await sub] received %u\n", as_u32(*r));
    });
    std::this_thread::sleep_for(50ms);  // let the waiter park in await()

    std::printf("publisher: write /sensor/temp = 23\n");
    (void)g.write(temp, value_u32(23));
    waiter.join();

    // Read back the last-known-value (a clone — keeps the segment alive for us).
    auto rb = g.read(temp);
    std::printf("read-back /sensor/temp = %u\n", rb ? as_u32(*rb) : 0u);

    // Field-write a QoS setting, then discover it via :schema.
    (void)g.write(*path_t::parse("/sensor/temp:settings.deadline_ns"), value_u32(5000));
    auto schema = g.read(*path_t::parse("/sensor/temp:schema"));
    if (schema) {
        auto point = tr::view::view_as_tlv(*schema);
        std::printf(":schema is a POINT with %zu children\n", point ? point->children.size() : 0u);
    }
    return 0;
}
