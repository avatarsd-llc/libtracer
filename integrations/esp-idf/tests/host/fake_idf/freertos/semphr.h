/**
 * @file
 * @brief Host stand-in for FreeRTOS's `semphr.h` — the COUNTING semaphore the
 *        TWAI link uses as its one TX backpressure point.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation. The real API spells all of
 * these as macros over `xQueue*`; functions are indistinguishable to the TU.
 *
 * The model behind them (fake_twai.cpp) is instrumented, and that instrumentation
 * is the point of the whole fake: it records how many callers are parked in
 * `xSemaphoreTake` AT ONCE, and the tick budget each take was given. A window
 * that is serialized behind a lock can never show more than one parked caller,
 * which is what makes #962 an observable rather than a code reading.
 *
 * The C spellings are FreeRTOS's, per the sibling fake_idf headers.
 */
#pragma once

#include "freertos/FreeRTOS.h"

/** @brief Opaque semaphore handle (FreeRTOS's incomplete-struct pointer). */
typedef struct fake_semaphore_t* SemaphoreHandle_t;

/** @brief Create a counting semaphore of @p max_count, starting at @p initial_count. */
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count, UBaseType_t initial_count);

/** @brief Free a semaphore. Callers waiting on it must already be gone. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

/**
 * @brief Take one count, waiting at most @p ticks_to_wait.
 * @return pdTRUE when a count was taken, pdFALSE on timeout.
 */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);

/**
 * @brief Give one count back.
 * @return pdTRUE when it was accepted, pdFALSE when the count was already at max.
 */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

/**
 * @brief Give one count back from ISR context.
 * @param higher_priority_task_woken Set to pdFALSE by this fake — see queue.h.
 * @return pdTRUE when it was accepted, pdFALSE when the count was already at max.
 */
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore,
                                 BaseType_t* higher_priority_task_woken);
