/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_transport.h` — the exact API surface
 *        `esp_ws_client_link.cpp` names, transcribed from
 *        `components/tcp_transport/include/esp_transport.h`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation: there is no socket and no TLS. The
 * model behind these declarations lives in fake_esp_transport.hpp, and it is deliberately
 * SOCKET-FREE — `esp_transport_get_socket` answers -1, so the link's best-effort
 * `TCP_NODELAY` is skipped and the host suite proves the receive path without opening a
 * POSIX socket anywhere (the #947 ruling: the ESP-IDF WebSocket plane never uses one).
 *
 * The C spellings here (a `typedef enum`, SCREAMING_SNAKE constants, no `_t` types of our
 * own) are IDF's, not this repo's — a fake that renamed them would not compile the TU it
 * exists to compile. Same latitude fake_idf/esp_http_server.h already takes.
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

/** @brief Opaque transport handle (IDF's incomplete-struct pointer). */
typedef struct esp_transport_item_t* esp_transport_handle_t;

/** @brief Free a transport handle. Does NOT free its parent. */
esp_err_t esp_transport_destroy(esp_transport_handle_t t);

/** @brief Open the connection (for the ws transport, the full RFC 6455 handshake).
 *         @return 0 on success, < 0 on failure. */
int esp_transport_connect(esp_transport_handle_t t, const char* host, int port, int timeout_ms);

/** @brief Read up to @p len payload bytes. @return bytes read, 0 on timeout (or a
 *         control frame consumed internally), < 0 on error. */
int esp_transport_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms);

/** @brief Wait for readability. @return 1 when data is available, 0 on timeout, < 0 on
 *         error. */
int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms);

/** @brief Write one message. @return bytes written, < 0 on error. */
int esp_transport_write(esp_transport_handle_t t, const char* buffer, int len, int timeout_ms);

/** @brief Close the connection (the handle stays valid for a re-connect). */
int esp_transport_close(esp_transport_handle_t t);

/** @brief The underlying socket descriptor, or -1 when the transport has none. */
int esp_transport_get_socket(esp_transport_handle_t t);
