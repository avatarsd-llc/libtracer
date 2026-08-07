/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_pthread.h` — the thread-stack sizing seam.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `std::thread` on IDF is a pthread, and its stack comes from the global
 * `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` unless `esp_pthread_set_cfg` has armed the
 * NEXT `pthread_create` on the calling thread. There is no host equivalent — glibc
 * threads are sized through `pthread_attr_t`, not a thread-local config — so the fake
 * does not resize anything. It RECORDS what was armed, which is exactly the observable
 * the #900 gate needs: a stack knob that never reaches this call is a knob that never
 * reaches the FreeRTOS task (fake_esp_transport.hpp: `armed_stack`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_transport.h"  // esp_err_t / ESP_OK

/** @brief IDF's per-thread pthread configuration. */
typedef struct {
    size_t stack_size;       /**< @brief Stack size of the next pthread. */
    size_t prio;             /**< @brief Its priority. */
    bool inherit_cfg;        /**< @brief Propagate this config further. */
    const char* thread_name; /**< @brief Its FreeRTOS task name. */
    int pin_to_core;         /**< @brief Core affinity. */
} esp_pthread_cfg_t;

/** @brief The compile-time defaults (a zeroed config in the fake). */
esp_pthread_cfg_t esp_pthread_get_default_config(void);

/** @brief Arm the NEXT `pthread_create` on this thread with @p cfg. */
esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* cfg);

/** @brief Read the config armed on this thread; leaves @p p untouched when none is. */
esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* p);
