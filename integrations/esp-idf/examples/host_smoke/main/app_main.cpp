/**
 * @file
 * @brief host_smoke — the libtracer P0 (in-process) profile on the ESP-IDF
 *        *linux* target (the POSIX host simulator).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is the host-target build/smoke for the integrations/esp-idf component.
 * It links the libtracer reference core (L0/L1 substrate, L2/L3 wire codec, L4
 * graph runtime) into an ESP-IDF `linux`-target app and drives the in-process
 * mirror surface a host_test consumer needs:
 *
 *   register a path  ->  write a value  ->  read it back
 *
 * Unlike examples/inprocess_mirror (esp32-only), this uses NO FreeRTOS tasks and
 * NO esp_log — only plain C++ and printf — so it builds and runs as a native
 * host executable under the `linux` target, which is exactly what a downstream
 * host_test suite exercises. The data path still bumps the L0 segment refcount
 * (tr::view::segment_ptr_t): every read() is a refcount clone of the same bytes,
 * never a byte copy.
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief Encode a little-endian u32 into a fresh heap segment, returned as a view. */
tr::view::view_t value_u32(std::uint32_t v) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i) {
        seg->bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
    return tr::view::view_t::over(std::move(seg));
}

std::uint32_t as_u32(const tr::view::view_t& view) {
    const auto b = view.bytes();
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < b.size() && i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[i])) << (8 * i);
    }
    return v;
}

}  // namespace

extern "C" void app_main(void) {
    std::printf("libtracer P0 host smoke (ESP-IDF linux target) starting\n");

    tr::graph::graph_t graph;

    // 1) Register a path (infallible on a literal — ADR-0056).
    const auto temp = graph.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // 2) Write a value.
    if (auto w = graph.write(temp, value_u32(23)); !w) {
        std::printf("FAIL: write\n");
        std::exit(1);
    }

    // 3) Read it back (a refcount-clone of the stored last-known-value).
    auto rb = graph.read(temp);
    if (!rb) {
        std::printf("FAIL: read\n");
        std::exit(1);
    }
    const std::uint32_t got = as_u32(rb->only());
    std::printf("read-back: /sensor/temp = %" PRIu32 "\n", got);
    if (got != 23) {
        std::printf("FAIL: read-back mismatch (expected 23)\n");
        std::exit(1);
    }

    std::printf("host smoke complete: register/write/read OK\n");
    std::exit(0);
}
