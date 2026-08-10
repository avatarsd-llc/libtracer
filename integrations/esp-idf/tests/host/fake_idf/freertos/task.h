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
 *
 * The WS server TU adds `uxTaskGetStackHighWaterMark`, which its #955 stack probe
 * samples. A host thread's stack is megabytes and its unused part is not filled with
 * a scannable pattern, so there is nothing here to measure — the fake reports whatever
 * @ref fake_stack_high_water_bytes holds, which is a healthy figure until a test says
 * otherwise. Its UNIT is ESP-IDF's, not vanilla FreeRTOS's: IDF returns BYTES and says
 * so in its own task.h ("in bytes (as opposed to words in the standard FreeRTOS
 * documentation)"), and the TU compares against a byte figure accordingly.
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

/**
 * @brief The free-stack figure (BYTES) the fake reports; a test writes it to stage a
 *        thin stack. Defaults to a plainly healthy 64 KiB.
 *
 * One instance across every TU that includes this header (a function-local static in an
 * inline function), so a test sets it without any target having to link an extra object.
 */
inline UBaseType_t& fake_stack_high_water_bytes() {
    static UBaseType_t bytes = 65536;
    return bytes;
}

/**
 * @brief How many times @ref uxTaskGetStackHighWaterMark has been called.
 *
 * The observable for a probe whose only other output is a log line: a link that has
 * LATCHED its report stops sampling, so the count is how a test tells "reported once and
 * fell silent" from "never tripped at all".
 */
inline unsigned& fake_stack_high_water_samples() {
    static unsigned n = 0;
    return n;
}

/** @brief Minimum free stack the task has ever had, in BYTES — IDF's unit (see the
 *         file brief). The handle is ignored: there is one host thread's answer. */
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t /*task*/) {
    ++fake_stack_high_water_samples();
    return fake_stack_high_water_bytes();
}

/** @brief Block the caller for @p ticks_to_delay scheduler ticks. */
void vTaskDelay(TickType_t ticks_to_delay);
