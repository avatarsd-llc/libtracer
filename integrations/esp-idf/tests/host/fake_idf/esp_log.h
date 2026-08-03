/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_log.h` — just enough for the chip TUs under
 *        test to log.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Part of the fake IDF surface the host teardown test compiles
 * `integrations/esp-idf/libtracer/httpd_ws_link.cpp` against (see fake_httpd.hpp).
 */
#pragma once

#include <cstdio>

/** @brief Warning log — the teardown paths under test log here; keep them visible. */
#define ESP_LOGW(tag, fmt, ...) std::fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
/** @brief Error log. */
#define ESP_LOGE(tag, fmt, ...) std::fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
/** @brief Info log. */
#define ESP_LOGI(tag, fmt, ...) std::fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
/** @brief Debug log — dropped on the host (the argument list is still type-checked). */
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
