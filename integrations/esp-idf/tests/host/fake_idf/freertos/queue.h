/**
 * @file
 * @brief Host stand-in for FreeRTOS's `queue.h` — the four calls the TWAI link
 *        uses for its ISR→dispatch handoff.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation: fixed-size items, a bounded
 * ring, a bounded receive. The real API spells `xQueueSendFromISR` as a macro; a
 * function is indistinguishable to the TU. The model behind these declarations
 * lives in fake_twai.cpp.
 *
 * The C spellings are FreeRTOS's, per the sibling fake_idf headers.
 */
#pragma once

#include "freertos/FreeRTOS.h"

/** @brief Opaque queue handle (FreeRTOS's incomplete-struct pointer). */
typedef struct fake_queue_t* QueueHandle_t;

/** @brief Create a queue of @p length items of @p item_size bytes, or NULL. */
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);

/** @brief Free a queue. Callers waiting on it must already be gone. */
void vQueueDelete(QueueHandle_t queue);

/**
 * @brief Pop one item into @p dst, waiting at most @p ticks_to_wait.
 * @return pdTRUE when an item was copied out, pdFALSE on timeout.
 */
BaseType_t xQueueReceive(QueueHandle_t queue, void* dst, TickType_t ticks_to_wait);

/**
 * @brief Push one item from ISR context; never blocks.
 * @param higher_priority_task_woken Set to pdFALSE by this fake — there is no
 *        host equivalent of a context switch on return from an interrupt.
 * @return pdTRUE when the item was queued, pdFALSE when the queue was full.
 */
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void* item,
                             BaseType_t* higher_priority_task_woken);
