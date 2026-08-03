/**
 * @file
 * @brief The host fake of `esp_http_server` — see fake_httpd.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "fake_httpd.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace fake_httpd {

namespace {

/** @brief The frame the current thread is delivering, consumed by httpd_ws_recv_frame's
 *         two-pass read (header pass, then payload pass) exactly as httpd's is. */
struct pending_frame_t {
    const std::byte* data = nullptr;
    std::size_t len = 0;
    bool final = true;
    httpd_ws_type_t type = HTTPD_WS_TYPE_BINARY;
};

thread_local pending_frame_t t_pending;

/** @brief The handler passed to the last httpd_register_uri_handler (see the header). */
esp_err_t (*g_handler)(httpd_req_t*) = nullptr;

}  // namespace

esp_err_t (*registered_handler())(httpd_req_t*) { return g_handler; }

void set_registered_handler(esp_err_t (*handler)(httpd_req_t*)) { g_handler = handler; }

server_t& instance() {
    static server_t server;
    return server;
}

void server_t::open_session(int fd) {
    const std::lock_guard lock(m_);
    sessions_[fd] = session_t{};
}

void* server_t::session_ctx(int fd) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    return it == sessions_.end() ? nullptr : it->second.ctx;
}

bool server_t::has_session(int fd) {
    const std::lock_guard lock(m_);
    return sessions_.find(fd) != sessions_.end();
}

std::size_t server_t::free_ctx_calls() const {
    const std::lock_guard lock(m_);
    return free_ctx_calls_;
}

std::size_t server_t::frames_sent() const {
    const std::lock_guard lock(m_);
    return frames_sent_;
}

void server_t::note_sent() {
    const std::lock_guard lock(m_);
    ++frames_sent_;
}

void server_t::set_queue_refusing(bool refusing) {
    const std::lock_guard lock(m_);
    queue_refusing_ = refusing;
}

void server_t::close_session(int fd) {
    // Lift the callback OUT of the lock before running it: httpd calls free_ctx from its
    // own task with no session lock held, and the link's handler re-enters this fake.
    void* ctx = nullptr;
    httpd_free_ctx_fn_t free_fn = nullptr;
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        ctx = it->second.ctx;
        free_fn = it->second.free_ctx;
        sessions_.erase(it);
        if (ctx != nullptr) ++free_ctx_calls_;
    }
    if (ctx != nullptr && free_fn != nullptr) free_fn(ctx);
}

void server_t::close_all() {
    std::vector<int> fds;
    {
        const std::lock_guard lock(m_);
        for (const auto& [fd, sess] : sessions_) fds.push_back(fd);
    }
    for (const int fd : fds) close_session(fd);
}

void server_t::set_ctx(int fd, void* ctx, httpd_free_ctx_fn_t free_fn) {
    // Transcribed from httpd_sess_set_ctx: on a CHANGE of ctx the previous context's
    // free_ctx runs inline, right here on the calling (server) task; then the new pair is
    // stored. Setting (nullptr, nullptr) therefore both fires and retires the callback.
    void* old_ctx = nullptr;
    httpd_free_ctx_fn_t old_free = nullptr;
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;  // session already gone: a no-op, as in IDF
        if (it->second.ctx != ctx) {
            old_ctx = it->second.ctx;
            old_free = it->second.free_ctx;
            it->second.ctx = ctx;
            if (old_ctx != nullptr) ++free_ctx_calls_;
        }
        it->second.free_ctx = free_fn;
    }
    if (old_ctx != nullptr && old_free != nullptr) old_free(old_ctx);
}

esp_err_t server_t::queue_work(httpd_work_fn_t work, void* arg) {
    const std::lock_guard lock(m_);
    if (queue_refusing_) return ESP_FAIL;
    queue_.push_back([work, arg]() { work(arg); });
    return ESP_OK;
}

esp_err_t server_t::trigger_close(int fd) {
    const std::lock_guard lock(m_);
    if (sessions_.find(fd) == sessions_.end()) return ESP_ERR_NOT_FOUND;
    if (queue_refusing_) return ESP_FAIL;
    queue_.push_back([this, fd]() { close_session(fd); });
    return ESP_OK;
}

void server_t::post(std::function<void()> action) {
    const std::lock_guard lock(m_);
    queue_.push_back(std::move(action));
}

std::size_t server_t::run_pending() {
    std::size_t ran = 0;
    for (;;) {
        std::function<void()> item;
        {
            const std::lock_guard lock(m_);
            if (queue_.empty()) break;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        item();  // unlocked: the work re-enters this fake and the link
        ++ran;
    }
    return ran;
}

httpd_ws_client_info_t server_t::fd_info(int fd) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return HTTPD_WS_CLIENT_INVALID;
    return it->second.websocket ? HTTPD_WS_CLIENT_WEBSOCKET : HTTPD_WS_CLIENT_HTTP;
}

esp_err_t server_t::deliver_frame(const httpd_uri_t& uri, int fd, std::span<const std::byte> body,
                                  bool final, httpd_ws_type_t type) {
    t_pending = pending_frame_t{body.data(), body.size(), final, type};
    httpd_req_t req = {};
    req.handle = static_cast<httpd_handle_t>(this);
    req.method = HTTP_POST;  // any non-GET: a data frame, not the opening handshake
    req.user_ctx = uri.user_ctx;
    req.fd = fd;
    const esp_err_t err = uri.handler(&req);
    t_pending = pending_frame_t{};
    return err;
}

}  // namespace fake_httpd

// ---------------------------------------------------------------------------
// The C API the chip TU calls — every one forwards to the single fake server.
// ---------------------------------------------------------------------------

const char* esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
    (void)config;
    *handle = static_cast<httpd_handle_t>(&fake_httpd::instance());
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
    (void)handle;
    fake_httpd::instance().close_all();
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri) {
    (void)handle;
    fake_httpd::set_registered_handler(uri->handler);
    return ESP_OK;
}

esp_err_t httpd_unregister_uri_handler(httpd_handle_t handle, const char* uri,
                                       httpd_method_t method) {
    (void)handle;
    (void)uri;
    (void)method;
    return ESP_OK;
}

int httpd_req_to_sockfd(httpd_req_t* req) { return req->fd; }

void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void* ctx, httpd_free_ctx_fn_t free_fn) {
    (void)handle;
    fake_httpd::instance().set_ctx(sockfd, ctx, free_fn);
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd) {
    (void)handle;
    return fake_httpd::instance().trigger_close(sockfd);
}

esp_err_t httpd_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void* arg) {
    (void)handle;
    return fake_httpd::instance().queue_work(work, arg);
}

esp_err_t httpd_ws_recv_frame(httpd_req_t* req, httpd_ws_frame_t* frame, std::size_t max_len) {
    (void)req;
    const auto& pending = fake_httpd::t_pending;
    frame->type = pending.type;
    frame->final = pending.final;
    frame->len = pending.len;
    if (max_len == 0) return ESP_OK;  // header pass, exactly as httpd's
    if (max_len < pending.len) return ESP_FAIL;
    if (pending.len != 0) std::memcpy(frame->payload, pending.data, pending.len);
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, httpd_ws_frame_t* frame) {
    (void)handle;
    (void)fd;
    (void)frame;
    fake_httpd::instance().note_sent();
    return ESP_OK;
}

httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t handle, int fd) {
    (void)handle;
    return fake_httpd::instance().fd_info(fd);
}
