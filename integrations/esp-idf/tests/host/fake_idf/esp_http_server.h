/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_http_server.h` — the exact API surface
 *        `httpd_ws_link.cpp` names, with the session table's REAL semantics.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This is a declaration-compatible fake, not a re-implementation of the server: there
 * is no socket, no select loop and no parser. What it does model exactly is the part
 * the teardown contract rests on, transcribed from
 * `components/esp_http_server/src/httpd_sess.c` and `httpd_main.c`:
 *   - `httpd_sess_set_ctx` runs the session's PREVIOUS `free_ctx` inline on a ctx
 *     change, then stores the new ctx/free_fn pair (so setting `(nullptr, nullptr)`
 *     both fires and retires the callback);
 *   - closing a session calls `free_ctx` once and nulls the stored ctx, so a second
 *     close is a no-op;
 *   - `httpd_queue_work` merely ENQUEUES; the work function runs later, on whichever
 *     thread drains the control queue (the fake's stand-in for the httpd task);
 *   - `httpd_sess_trigger_close` is that same asynchronous queue, not an inline close.
 * The behaviours a UAF hides behind are therefore reproducible on the host, under the
 * sanitizers, without silicon.
 */
#pragma once

#include <cstddef>
#include <cstdint>

/** @brief IDF's error type. */
typedef int esp_err_t;
#define ESP_OK 0            /**< @brief Success. */
#define ESP_FAIL (-1)       /**< @brief Generic failure. */
#define ESP_ERR_NOT_FOUND 5 /**< @brief No such session. */

/** @brief IDF's error-name helper (the TU logs with it). */
const char* esp_err_to_name(esp_err_t err);

/** @brief Opaque server handle. */
typedef void* httpd_handle_t;

/** @brief HTTP methods (only GET is named by the TU). */
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;

/** @brief Session context destructor, as httpd stores it. */
typedef void (*httpd_free_ctx_fn_t)(void* ctx);
/** @brief Control-queue work function. */
typedef void (*httpd_work_fn_t)(void* arg);

/** @brief A request as the URI handler sees it (the fields the TU reads). */
typedef struct httpd_req {
    httpd_handle_t handle; /**< @brief Server this request arrived on. */
    int method;            /**< @brief HTTP method; GET is the handshake. */
    void* user_ctx;        /**< @brief The `httpd_uri_t::user_ctx` (the link). */
    int fd;                /**< @brief Fake-only: the session socket (httpd hides this). */
} httpd_req_t;

/** @brief A URI registration. */
typedef struct httpd_uri {
    const char* uri;                      /**< @brief Path pattern. */
    httpd_method_t method;                /**< @brief Method to match. */
    esp_err_t (*handler)(httpd_req_t* r); /**< @brief The handler. */
    void* user_ctx;                       /**< @brief Passed through on every call. */
    bool is_websocket;                    /**< @brief WS upgrade route. */
    bool handle_ws_control_frames;        /**< @brief Deliver control frames too. */
} httpd_uri_t;

/** @brief Server configuration (the fields the owning ctor sets). */
typedef struct httpd_config {
    std::uint16_t server_port;      /**< @brief Listen port. */
    std::uint16_t ctrl_port;        /**< @brief Control UDP port. */
    std::size_t stack_size;         /**< @brief Server task stack. */
    std::uint16_t max_open_sockets; /**< @brief Socket budget. */
    bool lru_purge_enable;          /**< @brief Evict the oldest session at the cap. */
} httpd_config_t;

#define ESP_HTTPD_DEF_CTRL_PORT 32768 /**< @brief IDF's default control port. */
/** @brief IDF's config initialiser. */
#define HTTPD_DEFAULT_CONFIG()             \
    {.server_port = 80,                    \
     .ctrl_port = ESP_HTTPD_DEF_CTRL_PORT, \
     .stack_size = 4096,                   \
     .max_open_sockets = 7,                \
     .lru_purge_enable = false}

/** @brief WebSocket frame types. */
typedef enum {
    HTTPD_WS_TYPE_CONTINUE = 0x0,
    HTTPD_WS_TYPE_TEXT = 0x1,
    HTTPD_WS_TYPE_BINARY = 0x2,
    HTTPD_WS_TYPE_CLOSE = 0x8,
    HTTPD_WS_TYPE_PING = 0x9,
    HTTPD_WS_TYPE_PONG = 0xA
} httpd_ws_type_t;

/** @brief What a socket currently is, to the server. */
typedef enum {
    HTTPD_WS_CLIENT_INVALID = 0x0,
    HTTPD_WS_CLIENT_HTTP = 0x1,
    HTTPD_WS_CLIENT_WEBSOCKET = 0x2
} httpd_ws_client_info_t;

/** @brief One WebSocket frame. */
typedef struct httpd_ws_frame {
    bool final;            /**< @brief FIN bit. */
    bool fragmented;       /**< @brief Part of a fragmented message. */
    httpd_ws_type_t type;  /**< @brief Opcode. */
    std::uint8_t* payload; /**< @brief Caller-provided buffer (recv) / bytes (send). */
    std::size_t len;       /**< @brief Payload length. */
} httpd_ws_frame_t;

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri);
esp_err_t httpd_unregister_uri_handler(httpd_handle_t handle, const char* uri,
                                       httpd_method_t method);
int httpd_req_to_sockfd(httpd_req_t* req);
void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void* ctx, httpd_free_ctx_fn_t free_fn);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);
esp_err_t httpd_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void* arg);
esp_err_t httpd_ws_recv_frame(httpd_req_t* req, httpd_ws_frame_t* frame, std::size_t max_len);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, httpd_ws_frame_t* frame);
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t handle, int fd);
