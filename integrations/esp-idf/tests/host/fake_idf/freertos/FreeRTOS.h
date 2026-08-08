/**
 * @file
 * @brief Host stand-in for FreeRTOS's umbrella header — the base types and the
 *        tick conversion, nothing else.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ESP-IDF requires `freertos/FreeRTOS.h` before `freertos/task.h`, `queue.h` or
 * `semphr.h`; the chip TUs under test follow that rule, so the fake surface must
 * offer the same header. What lives here is what the real header supplies through
 * `projdefs.h` / `portmacro.h`: the width types, the boolean returns, and
 * `pdMS_TO_TICKS`. The primitives themselves live in the fake `queue.h` /
 * `semphr.h` / `task.h`.
 *
 * `configTICK_RATE_HZ` is IDF's own default of 100, and it is LOAD-BEARING, not
 * decoration: `pdMS_TO_TICKS` truncates, so at 100 Hz a 1–9 ms timeout floors to
 * zero ticks. `twai_link.cpp::tx_wait_ticks` exists precisely to catch that, and a
 * fake that quietly used a 1000 Hz tick would make its guard unobservable.
 *
 * The C spellings here are FreeRTOS's, not this repo's — a fake that renamed them
 * would not compile the TUs it exists to compile.
 */
#pragma once

#include <stdint.h>

/** @brief FreeRTOS's signed return/width type. */
typedef int BaseType_t;
/** @brief Its unsigned counterpart (counts, depths, priorities). */
typedef unsigned int UBaseType_t;
/** @brief A count of scheduler ticks. */
typedef uint32_t TickType_t;

#ifndef pdTRUE
#define pdTRUE ((BaseType_t)1) /**< @brief Success / true. */
#endif
#ifndef pdFALSE
#define pdFALSE ((BaseType_t)0) /**< @brief Failure / false. */
#endif
#ifndef pdPASS
#define pdPASS pdTRUE /**< @brief Spelling the queue/semaphore API prefers. */
#endif
#ifndef pdFAIL
#define pdFAIL pdFALSE /**< @brief Its negative. */
#endif

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 100 /**< @brief IDF's default tick rate. */
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFU) /**< @brief Block forever. */
#endif

#ifndef pdMS_TO_TICKS
/** @brief Milliseconds to ticks, TRUNCATING exactly as FreeRTOS does. */
#define pdMS_TO_TICKS(ms) \
    ((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000U))
#endif
