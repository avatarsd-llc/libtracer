/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_transport_tcp.h` — the one entry point
 *        `esp_ws_client_link.cpp` names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

#include "esp_transport.h"

/** @brief Create the parent TCP transport. The fake opens no socket. */
esp_transport_handle_t esp_transport_tcp_init(void);
