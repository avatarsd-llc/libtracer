/**
 * @file
 * @brief The host fake of `esp_http_server`'s session table and control work queue —
 *        the driver the adopted-mode teardown test steers.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The declarations the chip TU compiles against live in `fake_idf/esp_http_server.h`;
 * this is the model behind them, plus the levers a test needs that the real API has no
 * business exposing: open a session, deliver a frame INTO the URI handler, drain (or
 * refuse to drain) the control queue, close a session, and count how often a session's
 * `free_ctx` has been run.
 *
 * The one rule the fake keeps religiously, because the bug under test lives on it: the
 * fake never holds its own lock while calling into the link (a `free_ctx` re-enters the
 * link's peer mutex), so a lock inversion in the fake cannot masquerade as a finding.
 */
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>

#include "esp_http_server.h"

namespace fake_httpd {

/** @brief One entry of the fake session table — httpd's `struct sock_db`, trimmed. */
struct session_t {
    void* ctx = nullptr;                    /**< @brief User context. */
    httpd_free_ctx_fn_t free_ctx = nullptr; /**< @brief Its destructor. */
    bool websocket = true;                  /**< @brief Reported by ws_get_fd_info. */
};

/** @brief The fake server: one session table, one control queue, no sockets. */
class server_t {
   public:
    /** @brief Admit a socket into the session table (the accept the fake skips). */
    void open_session(int fd);
    /** @brief The context currently stored for @p fd (nullptr if none / no session). */
    [[nodiscard]] void* session_ctx(int fd);
    /** @brief True while @p fd is in the session table. */
    [[nodiscard]] bool has_session(int fd);
    /** @brief Close @p fd: run its `free_ctx` once (if any) and drop the entry. */
    void close_session(int fd);
    /** @brief Close every remaining session — the adopting server's own teardown. */
    void close_all();
    /** @brief How many times a session context destructor has been RUN, ever. */
    [[nodiscard]] std::size_t free_ctx_calls() const;

    /** @brief Enqueue a test action onto the same control queue `httpd_queue_work` uses,
     *         so it runs on whichever thread drains it (the fake's "httpd task"). */
    void post(std::function<void()> action);
    /** @brief Run every queued item; returns how many ran. Call from the server thread. */
    std::size_t run_pending();

    /**
     * @brief Deliver one WebSocket frame into @p uri's handler, as the server task would.
     * @return The handler's verdict (ESP_FAIL => the server would close the socket).
     */
    esp_err_t deliver_frame(const httpd_uri_t& uri, int fd, std::span<const std::byte> body,
                            bool final = true, httpd_ws_type_t type = HTTPD_WS_TYPE_BINARY);

    /** @brief How many frames the link has successfully sent through the fake. */
    [[nodiscard]] std::size_t frames_sent() const;

    // --- the surface the free functions in fake_httpd.cpp forward to ---
    void set_ctx(int fd, void* ctx, httpd_free_ctx_fn_t free_fn);
    esp_err_t queue_work(httpd_work_fn_t work, void* arg);
    esp_err_t trigger_close(int fd);
    [[nodiscard]] httpd_ws_client_info_t fd_info(int fd);
    void note_sent();
    /** @brief Refuse further `httpd_queue_work` calls — the ctrl-queue-full failure. */
    void set_queue_refusing(bool refusing);

   private:
    mutable std::mutex m_;
    std::map<int, session_t> sessions_;
    std::deque<std::function<void()>> queue_;
    std::size_t free_ctx_calls_ = 0;
    std::size_t frames_sent_ = 0;
    bool queue_refusing_ = false;
};

/** @brief The one fake server; its address IS the `httpd_handle_t` the link adopts. */
server_t& instance();

/**
 * @brief The URI handler most recently registered, kept even across an unregister.
 *
 * The real server drops the route on unregister, but a frame it had ALREADY dispatched
 * is still inside that handler — the race the teardown gate exists for. Retaining the
 * pointer is how a test reproduces that frame deterministically.
 */
esp_err_t (*registered_handler())(httpd_req_t*);

/** @brief Record the handler a registration installed (called by the fake C API). */
void set_registered_handler(esp_err_t (*handler)(httpd_req_t*));

}  // namespace fake_httpd
