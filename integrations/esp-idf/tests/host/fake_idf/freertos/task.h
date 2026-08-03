/**
 * @file
 * @brief Host stand-in for FreeRTOS's `task.h` — task IDENTITY only.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The chip TU under test uses exactly one FreeRTOS facility: "which task am I on?",
 * to decide whether the teardown detach must be queued to the server task or run
 * inline on it (the #814 deadlock). A host thread stands in for a task, so a
 * thread-local address is a faithful identity token.
 */
#pragma once

/** @brief Opaque task identity, as FreeRTOS spells it. */
typedef void* TaskHandle_t;

/** @brief The calling thread's identity — distinct per thread, stable within one. */
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
    static thread_local char self = 0;
    return static_cast<TaskHandle_t>(&self);
}
