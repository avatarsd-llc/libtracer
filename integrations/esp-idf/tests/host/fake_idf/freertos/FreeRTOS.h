/**
 * @file
 * @brief Host stand-in for FreeRTOS's umbrella header — empty by design.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ESP-IDF requires `freertos/FreeRTOS.h` before `freertos/task.h`; the chip TU under
 * test follows that rule, so the fake surface must offer the same header. Everything
 * the TU actually uses lives in the fake `freertos/task.h`.
 */
#pragma once
