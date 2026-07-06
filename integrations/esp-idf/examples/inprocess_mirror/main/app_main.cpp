/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * inprocess_mirror — the libtracer P0 (in-process) profile on an ESP32-C6.
 *
 * This is the on-silicon build/smoke target for the integrations/esp-idf
 * component. It links the libtracer reference core (L0/L1 substrate, L2/L3 wire
 * codec, L4 graph runtime) into a real ESP-IDF app and drives the in-process
 * mirror surface that strawberry-fw migration Phase 1 (ADR-0071) needs:
 *
 *   register a path  ->  write a value  ->  read it back  ->  await the next write
 *
 * The data path bumps the L0 segment refcount (tr::view::segment_ptr_t, the
 * <atomic> ref_count_t in segment.hpp): every read()/fan-out is a refcount clone
 * of the same bytes, never a byte copy. Exercising it here proves that atomic
 * refcount path links and runs on single-core FreeRTOS.
 *
 * This is example code, not core: it intentionally uses plain ESP_LOG comments
 * rather than the core's Doxygen style.
 */

#include <chrono>
#include <cinttypes>
#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libtracer/tracer.hpp"

namespace {

constexpr const char* kTag = "inprocess_mirror";

using tr::graph::path_t;
using tr::graph::role_t;

// The shared in-process graph and the pinned vertex handle, resolved once.
tr::graph::graph_t g_graph;
tr::graph::vertex_t* g_temp = nullptr;

// Encode a little-endian u32 into a fresh heap segment, returned as a view.
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

// A FreeRTOS task that parks in await() and reports the next value written.
void await_task(void*) {
    ESP_LOGI(kTag, "await: parking on /sensor/temp (2s timeout)");
    auto r = g_graph.await(g_temp, std::chrono::seconds(2));
    if (r) {
        ESP_LOGI(kTag, "await: woke with value = %" PRIu32, as_u32(r->only()));
    } else {
        ESP_LOGW(kTag, "await: timed out with no write");
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "libtracer P0 in-process mirror starting");

    // 1) Register a path.
    auto reg = g_graph.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    if (!reg) {
        ESP_LOGE(kTag, "register_vertex failed");
        return;
    }
    g_temp = *reg;

    // 2) Spawn the await waiter, then let it park before the write.
    xTaskCreate(await_task, "tr_await", 4096, nullptr, 5, nullptr);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3) Write a value.
    ESP_LOGI(kTag, "write: /sensor/temp = 23");
    if (auto w = g_graph.write(g_temp, value_u32(23)); !w) {
        ESP_LOGE(kTag, "write failed");
        return;
    }

    // 4) Read it back (a refcount-clone of the stored last-known-value).
    if (auto rb = g_graph.read(g_temp); rb) {
        ESP_LOGI(kTag, "read-back: /sensor/temp = %" PRIu32, as_u32(rb->only()));
    } else {
        ESP_LOGE(kTag, "read failed");
    }

    // Let the await task drain its log line before app_main returns.
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(kTag, "in-process mirror smoke complete");
}
