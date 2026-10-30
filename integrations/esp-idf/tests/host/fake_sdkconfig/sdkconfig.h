/**
 * @file
 * @brief Host stand-in for an ESP-IDF chip build's generated `sdkconfig.h` (#2416).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Empty on purpose: it defines no `CONFIG_IDF_TARGET_LINUX`, so with `ESP_PLATFORM` set
 * `static_thread.hpp` selects its FreeRTOS branch, and the `CONFIG_PTHREAD_TASK_*` defaults
 * fall back to the values `core/src/static_thread.cpp` states. Its own directory, not
 * fake_idf/, so no other host suite starts seeing an sdkconfig.h it never had.
 */
#pragma once
