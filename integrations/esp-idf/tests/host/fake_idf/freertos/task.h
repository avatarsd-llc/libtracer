/**
 * @file
 * @brief Host stand-in for FreeRTOS's `task.h` — task IDENTITY and the yielding
 *        delay.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The WS server TU uses exactly one FreeRTOS facility here: "which task am I on?",
 * to decide whether the teardown detach must be queued to the server task or run
 * inline on it (the #814 deadlock). A host thread stands in for a task, so a
 * thread-local address is a faithful identity token.
 *
 * The TWAI TU adds `vTaskDelay`, which its teardown drain uses instead of a spin
 * so a higher-priority destroying task cannot starve the writers it is waiting for
 * on a unicore chip (#962). A host thread sleep reproduces the only property that
 * matters to the suite: the caller stops holding the CPU for a tick.
 */
#pragma once

#include "freertos/FreeRTOS.h"

/** @brief Opaque task identity, as FreeRTOS spells it. */
typedef void* TaskHandle_t;

/** @brief The calling thread's identity — distinct per thread, stable within one. */
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
    static thread_local char self = 0;
    return static_cast<TaskHandle_t>(&self);
}

/** @brief Block the caller for @p ticks_to_delay scheduler ticks. */
void vTaskDelay(TickType_t ticks_to_delay);
