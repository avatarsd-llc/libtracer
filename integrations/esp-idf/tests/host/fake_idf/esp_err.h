/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_err.h` — the error type every IDF
 *        header in this fake surface names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Split out of the fake `esp_transport.h` so a suite that compiles a chip TU with
 * no TCP transport in it (the TWAI link) does not have to drag the WebSocket
 * fake along for one typedef. Only the codes the fakes and the TUs under test
 * actually name are listed; IDF's real header is a long table.
 *
 * The C spellings here (SCREAMING_SNAKE macros, no `_t` types of our own) are
 * IDF's, not this repo's — same latitude the sibling fake_idf headers take.
 */
#pragma once

/** @brief IDF's error type. */
typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK 0 /**< @brief Success. */
#endif
#ifndef ESP_FAIL
#define ESP_FAIL (-1) /**< @brief Generic failure. */
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102 /**< @brief An argument was not acceptable. */
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103 /**< @brief The object was in the wrong state. */
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105 /**< @brief Nothing was registered / nothing to report. */
#endif
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT 0x107 /**< @brief The bounded wait expired. */
#endif
