/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_freertos_hooks.h` — the per-core idle hook table.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The httpd WS link installs one idle hook per core and waits on it when its ingress drain
 * budget is spent (ADR-0085). On the target the idle task runs the hooks each time the core
 * has nothing else to do. The host has no idle task, so the fake keeps the table and a test
 * runs the hooks by hand with `fake_httpd::run_idle_hooks` — which is what makes "the drain
 * resumes only once the core has idled" a deterministic step instead of a race.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** @brief An idle hook: true lets the core enter its low-power wait. */
typedef bool (*esp_freertos_idle_cb_t)(void);

/**
 * @brief Install @p new_idle_cb on core @p cpuid.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a core that does not exist, or ESP_ERR_NO_MEM
 *         when that core's table (eight entries, as IDF's) is full.
 */
esp_err_t esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t new_idle_cb,
                                                  UBaseType_t cpuid);

/** @brief Remove @p old_idle_cb from core @p cpuid, if it is there. */
void esp_deregister_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t old_idle_cb,
                                               UBaseType_t cpuid);
