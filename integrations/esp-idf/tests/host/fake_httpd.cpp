/**
 * @file
 * @brief The host fake of `esp_http_server` — see fake_httpd.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "fake_httpd.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstdint>
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

/**
 * @brief The request the calling thread is currently servicing — httpd's `hd_req` /
 *        `hd_req_aux.sd` pair, which is what makes `httpd_sess_set_ctx` behave
 *        differently for the session being serviced than for any other.
 */
struct request_scope_t {
    bool active = false;                    /**< @brief A handler frame is on this stack. */
    int fd = -1;                            /**< @brief The session it is servicing. */
    void* sess_ctx = nullptr;               /**< @brief `httpd_req_t::sess_ctx`. */
    httpd_free_ctx_fn_t free_ctx = nullptr; /**< @brief `httpd_req_t::free_ctx`. */
};

thread_local request_scope_t t_req;

/** @brief The route the last httpd_register_uri_handler installed (see the header). */
route_t g_route;

}  // namespace

route_t registered_route() { return g_route; }

void set_registered_route(route_t route) { g_route = route; }

httpd_config_t& start_config_slot() {
    static httpd_config_t cfg = {};
    return cfg;
}

const httpd_config_t& last_start_config() { return start_config_slot(); }

server_t& instance() {
    static server_t server;
    return server;
}

bool server_t::open_session(int fd, void* ctx, httpd_free_ctx_fn_t free_fn) {
    // The pre-handshake predicate first, and OUTSIDE m_ — it re-enters the link (the
    // gate's mutex), and the fake never holds its own lock across a call into the link.
    // The request it gets is the opening GET as httpd_uri.c hands it over: the
    // registration's user_ctx already attached, method GET, on this socket.
    const route_t route = g_route;
    if (route.pre_handshake != nullptr) {
        httpd_req_t req = {};
        req.handle = static_cast<httpd_handle_t>(this);
        req.method = HTTP_GET;
        req.user_ctx = route.user_ctx;
        req.fd = fd;
        if (route.pre_handshake(&req) != ESP_OK) return false;  // no 101, no session
    }
    const std::lock_guard lock(m_);
    session_t sess;
    sess.ctx = ctx;
    sess.free_ctx = free_fn;
    // The handshake latches the route INTO the session; nothing later can revoke it.
    sess.ws_handler = route.handler;
    sess.ws_user_ctx = route.user_ctx;
    // httpd_sess_new seeds the session from the server's clock WITHOUT advancing it, so a
    // brand-new session starts level with the oldest existing one rather than ahead of it.
    sess.lru_counter = lru_clock_;
    sessions_[fd] = sess;
    return true;
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

void server_t::set_queue_capacity(std::size_t cap) {
    const std::lock_guard lock(m_);
    queue_cap_ = cap;
}

std::size_t server_t::queue_depth() const {
    const std::lock_guard lock(m_);
    return queue_.size();
}

std::size_t server_t::queue_drops() const {
    const std::lock_guard lock(m_);
    return queue_drops_;
}

void server_t::set_send_script(int fd, std::vector<send_result_t> script) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    it->second.script = std::move(script);
    it->second.script_pos = 0;
}

std::size_t server_t::writes(int fd) const {
    const std::lock_guard lock(m_);
    const auto it = writes_.find(fd);
    return it == writes_.end() ? 0 : it->second;
}

esp_err_t server_t::set_send_override(int fd, httpd_send_func_t send_fn) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return ESP_ERR_NOT_FOUND;
    it->second.send_fn = send_fn;
    return ESP_OK;
}

bool server_t::owns_socket(int fd) const {
    const std::lock_guard lock(m_);
    return sessions_.find(fd) != sessions_.end();
}

esp_err_t server_t::update_lru_counter(int fd) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return ESP_ERR_NOT_FOUND;
    it->second.lru_counter = ++lru_clock_;
    return ESP_OK;
}

std::uint64_t server_t::lru_counter(int fd) const {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    return it == sessions_.end() ? 0 : it->second.lru_counter;
}

int server_t::lowest_lru_fd() const {
    const std::lock_guard lock(m_);
    int victim = -1;
    std::uint64_t lowest = UINT64_MAX;
    for (const auto& [fd, sess] : sessions_)
        if (sess.lru_counter < lowest) {
            lowest = sess.lru_counter;
            victim = fd;
        }
    return victim;
}

bool server_t::is_shut(int fd) const {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    return it != sessions_.end() && it->second.shut;
}

int server_t::raw_shutdown(int fd) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        errno = ENOTCONN;
        return -1;
    }
    it->second.shut = true;
    return 0;
}

std::size_t server_t::reap_shut() {
    // httpd_server's select arm: a socket that `shutdown` made readable-at-EOF comes back
    // from select, httpd_sess_process reads 0 bytes and deletes the session. No control
    // message is involved anywhere on this path — which is exactly why the link can rely
    // on it while the control queue is jammed.
    std::vector<int> fds;
    {
        const std::lock_guard lock(m_);
        for (const auto& [fd, sess] : sessions_)
            if (sess.shut) fds.push_back(fd);
    }
    for (const int fd : fds) close_session(fd);  // runs free_ctx, unlocked
    return fds.size();
}

int server_t::raw_send(int fd, std::size_t buf_len) {
    const std::lock_guard lock(m_);
    ++writes_[fd];
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        errno = EBADF;
        return -1;
    }
    session_t& sess = it->second;
    if (sess.shut) {
        // lwIP fails a write on a shut socket AT ONCE — it does not wait out SO_SNDTIMEO,
        // which is half of why `shutdown` is the lever that unjams a starved task.
        errno = EPIPE;
        return -1;
    }
    send_result_t outcome = send_result_t::FULL;
    if (!sess.script.empty()) {
        const std::size_t i =
            sess.script_pos < sess.script.size() ? sess.script_pos : sess.script.size() - 1;
        outcome = sess.script[i];
        ++sess.script_pos;  // the LAST entry repeats: a stalled peer stays stalled
    }
    switch (outcome) {
        case send_result_t::FULL:
            return static_cast<int>(buf_len);
        case send_result_t::TIMEOUT:
            errno = EAGAIN;  // lwIP: SO_SNDTIMEO expired with NOTHING written
            return -1;
        case send_result_t::SHORT:
            break;
    }
    // The bound expired MID-buffer: lwIP returns the PARTIAL count, not an error. Half a
    // WebSocket frame is on the wire and the stream is desynchronised from here on.
    return static_cast<int>(buf_len / 2);
}

/** @brief httpd's own default send function (`httpd_default_send`): the raw write plus
 *         IDF's errno-to-HTTPD_SOCK_ERR mapping. Used for a session with no override. */
[[nodiscard]] int default_send_fn(httpd_handle_t hd, int fd, const char* buf, std::size_t buf_len,
                                  int flags) {
    (void)flags;
    if (buf == nullptr) return HTTPD_SOCK_ERR_INVALID;
    const int ret = static_cast<server_t*>(hd)->raw_send(fd, buf_len);
    if (ret < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT
                                                       : HTTPD_SOCK_ERR_FAIL;
    return ret;
}

int server_t::socket_send(int fd, const char* buf, std::size_t buf_len, int flags) {
    httpd_send_func_t send_fn = nullptr;
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end()) return -1;
        send_fn = it->second.send_fn;
    }
    // Unlocked: an override re-enters the link (and the link re-enters this fake).
    if (send_fn == nullptr) send_fn = &default_send_fn;
    return send_fn(static_cast<httpd_handle_t>(this), fd, buf, buf_len, flags);
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

void* server_t::get_ctx(int fd) {
    // httpd_sess_get_ctx: inside the handler servicing this very session the live value
    // is the REQUEST's, not the socket table's.
    if (t_req.active && t_req.fd == fd) return t_req.sess_ctx;
    return session_ctx(fd);
}

void server_t::set_ctx(int fd, void* ctx, httpd_free_ctx_fn_t free_fn) {
    // Transcribed from httpd_sess_set_ctx (httpd_sess.c:280-311). Two branches, and the
    // difference between them is a use-after-free:
    //   - REQUEST-SCOPED — called from inside the handler servicing THIS session: only
    //     the in-flight httpd_req_t is edited. The socket table keeps its ctx, and the
    //     previous context is NOT freed here ("it will be freed inside
    //     httpd_req_cleanup()") — i.e. after the handler returns;
    //   - otherwise the socket table is edited and a CHANGE of ctx runs the previous
    //     free_ctx inline, right here on the calling (server) task.
    if (t_req.active && t_req.fd == fd) {
        void* stale = nullptr;
        httpd_free_ctx_fn_t stale_free = nullptr;
        if (t_req.sess_ctx != ctx) {
            // Only a context the socket table does NOT also hold is freed here.
            if (session_ctx(fd) != t_req.sess_ctx) {
                stale = t_req.sess_ctx;
                stale_free = t_req.free_ctx;
                if (stale != nullptr) {
                    const std::lock_guard lock(m_);
                    ++free_ctx_calls_;
                }
            }
            t_req.sess_ctx = ctx;
        }
        t_req.free_ctx = free_fn;
        if (stale != nullptr && stale_free != nullptr) stale_free(stale);
        return;
    }
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

/**
 * @brief The shared enqueue behind `httpd_queue_work` and `httpd_sess_trigger_close` —
 *        ONE control socket, and both of its failure modes OBSERVABLE.
 *
 * Transcribed from `httpd_queue_work` (httpd_main.c) as it stands at the ESP-IDF floor the
 * component requires, `>=5.5.5`: the mbox slot is reserved through a counting semaphore
 * sized `CONFIG_LWIP_UDP_RECVMBOX_SIZE` BEFORE the `sendto`, so a full control queue is an
 * `ESP_FAIL` the caller sees, exactly as a refusal from `cs_send_to_ctrl_sock` is.
 *
 * The fake used to model the older shape as well — an enqueue past the mbox discarded
 * inside lwIP while `sendto` still returned success, which no caller could distinguish
 * from "queued". That mode is GONE with the floor (#949): keeping a lever for a condition
 * the supported ESP-IDF versions cannot produce would only let a suite prove the link
 * survives something it will never meet.
 *
 * @pre `m_` is held.
 * @retval false The caller must report ESP_FAIL — a full queue, or the explicit refusal.
 */
bool server_t::enqueue_locked(std::function<void()> item) {
    if (queue_refusing_) return false;
    if (queue_cap_ != 0 && queue_.size() >= queue_cap_) {
        ++queue_drops_;  // the counting semaphore could not be taken
        return false;
    }
    queue_.push_back(std::move(item));
    return true;
}

esp_err_t server_t::queue_work(httpd_work_fn_t work, void* arg) {
    const std::lock_guard lock(m_);
    if (!enqueue_locked([work, arg]() { work(arg); })) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t server_t::trigger_close(int fd) {
    const std::lock_guard lock(m_);
    if (sessions_.find(fd) == sessions_.end()) return ESP_ERR_NOT_FOUND;
    // `httpd_sess_trigger_close` IS `httpd_queue_work(httpd_sess_close, sd)`
    // (httpd_sess.c:476-481) — the same socket, the same queue, the same refusal.
    if (!enqueue_locked([this, fd]() { close_session(fd); })) return ESP_FAIL;
    return ESP_OK;
}

void server_t::post(std::function<void()> action) {
    const std::lock_guard lock(m_);
    (void)enqueue_locked(std::move(action));
}

bool server_t::run_one() {
    std::function<void()> item;
    {
        const std::lock_guard lock(m_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
    }
    item();  // unlocked: the work re-enters this fake and the link
    return true;
}

std::size_t server_t::run_pending() {
    std::size_t ran = 0;
    while (run_one()) ++ran;
    return ran + reap_shut();
}

httpd_ws_client_info_t server_t::fd_info(int fd) {
    const std::lock_guard lock(m_);
    const auto it = sessions_.find(fd);
    if (it == sessions_.end()) return HTTPD_WS_CLIENT_INVALID;
    return it->second.websocket ? HTTPD_WS_CLIENT_WEBSOCKET : HTTPD_WS_CLIENT_HTTP;
}

void server_t::set_frame_hook(std::function<void()> hook) {
    const std::lock_guard lock(m_);
    frame_hook_ = std::move(hook);
}

void server_t::run_frame_hook() {
    std::function<void()> hook;
    {
        const std::lock_guard lock(m_);
        hook = frame_hook_;
    }
    if (hook) hook();  // unlocked: the hook drives a concurrent teardown
}

esp_err_t server_t::deliver_frame(int fd, std::span<const std::byte> body, bool final,
                                  httpd_ws_type_t type) {
    // The route comes from the SESSION (httpd_parse.c:796,824), so this dispatches
    // whether or not the URI is still registered — the reachability an unregister does
    // not close.
    esp_err_t (*handler)(httpd_req_t*) = nullptr;
    httpd_req_t req = {};
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it == sessions_.end() || it->second.ws_handler == nullptr) return ESP_ERR_NOT_FOUND;
        handler = it->second.ws_handler;
        req.user_ctx = it->second.ws_user_ctx;
        // httpd_req_new: the session's context pair is copied INTO the request.
        t_req = request_scope_t{true, fd, it->second.ctx, it->second.free_ctx};
    }
    t_pending = pending_frame_t{body.data(), body.size(), final, type};
    req.handle = static_cast<httpd_handle_t>(this);
    req.method = HTTP_POST;  // any non-GET: a data frame, not the opening handshake
    req.fd = fd;
    const esp_err_t err = handler(&req);
    t_pending = pending_frame_t{};

    // httpd_req_cleanup (httpd_parse.c:733-735): a socket-table ctx that no longer
    // matches the request's is freed HERE, with the STORED destructor, after the handler
    // has returned — the call an in-call teardown cannot be present for.
    void* stale = nullptr;
    httpd_free_ctx_fn_t stale_free = nullptr;
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it != sessions_.end()) {
            if (it->second.ctx != t_req.sess_ctx) {
                stale = it->second.ctx;
                stale_free = it->second.free_ctx;
                if (stale != nullptr) ++free_ctx_calls_;
            }
            it->second.ctx = t_req.sess_ctx;
            it->second.free_ctx = t_req.free_ctx;
        }
    }
    t_req = request_scope_t{};
    if (stale != nullptr && stale_free != nullptr) stale_free(stale);
    // httpd_sess_process's tail (httpd_sess.c:438): INBOUND processing is what advances a
    // session's LRU counter. Modelling it is what makes the asymmetry the #955 mitigation
    // addresses reproducible — without it every session would sit at 0 and the victim
    // search would answer by map order instead of by traffic.
    {
        const std::lock_guard lock(m_);
        const auto it = sessions_.find(fd);
        if (it != sessions_.end()) it->second.lru_counter = ++lru_clock_;
    }
    return err;
}

}  // namespace fake_httpd

// ---------------------------------------------------------------------------
// The C API the chip TU calls — every one forwards to the single fake server.
// ---------------------------------------------------------------------------

const char* esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
    if (config != nullptr) fake_httpd::start_config_slot() = *config;
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
    fake_httpd::set_registered_route({uri->handler, uri->user_ctx, uri->ws_pre_handshake_cb});
    return ESP_OK;
}

esp_err_t httpd_unregister_uri_handler(httpd_handle_t handle, const char* uri,
                                       httpd_method_t method) {
    (void)handle;
    (void)uri;
    (void)method;
    // Drops the route for FUTURE handshakes only. Sessions that already latched it keep
    // dispatching — httpd_uri.c never walks the session table on unregister.
    fake_httpd::set_registered_route({});
    return ESP_OK;
}

int httpd_req_to_sockfd(httpd_req_t* req) { return req->fd; }

void* httpd_sess_get_ctx(httpd_handle_t handle, int sockfd) {
    (void)handle;
    return fake_httpd::instance().get_ctx(sockfd);
}

void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void* ctx, httpd_free_ctx_fn_t free_fn) {
    (void)handle;
    fake_httpd::instance().set_ctx(sockfd, ctx, free_fn);
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd) {
    (void)handle;
    return fake_httpd::instance().trigger_close(sockfd);
}

esp_err_t httpd_sess_update_lru_counter(httpd_handle_t handle, int sockfd) {
    (void)handle;
    return fake_httpd::instance().update_lru_counter(sockfd);
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
    // The payload pass: the handler is inside the link but has claimed nothing yet — the
    // window the destructor's handler barrier exists to cover.
    fake_httpd::instance().run_frame_hook();
    if (max_len < pending.len) return ESP_FAIL;
    if (pending.len != 0) std::memcpy(frame->payload, pending.data, pending.len);
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, httpd_ws_frame_t* frame) {
    (void)handle;
    // httpd_ws.c writes ONE frame as TWO calls to the session's send fn — the header, then
    // the payload — and calls the send a success on ANY non-negative return
    // (httpd_ws.c:447-458, release/v5.5). Transcribed as-is, both halves: the fake must
    // reproduce the bug, not the intent. The non-negative test is what turns a short write
    // into a silently-lost half frame, and the SPLIT is what lets a frame be announced on
    // the wire and then abandoned, with one indistinguishable ESP_FAIL for both (#951).
    //
    // A server-to-client frame is never masked, so the header is 2 bytes plus the extended
    // length — 2 more for 126..65535, 8 more above. Its BYTES are not modelled (the fake's
    // socket layer only ever sees a length); its length is, because that is what a peer
    // has been promised once the write lands.
    std::size_t header_len = 2;
    if (frame->len > 0xFFFFU)
        header_len = 10;
    else if (frame->len >= 126U)
        header_len = 4;
    const std::uint8_t header[10] = {};
    if (fake_httpd::instance().socket_send(fd, reinterpret_cast<const char*>(header), header_len,
                                           0) < 0)
        return ESP_FAIL;
    // Exactly httpd_ws.c's guard: an empty frame is header-only, so it takes ONE write.
    if (frame->len > 0 && frame->payload != nullptr) {
        const int ret = fake_httpd::instance().socket_send(
            fd, reinterpret_cast<const char*>(frame->payload), frame->len, 0);
        if (ret < 0) return ESP_FAIL;
    }
    fake_httpd::instance().note_sent();
    return ESP_OK;
}

esp_err_t httpd_sess_set_send_override(httpd_handle_t handle, int sockfd,
                                       httpd_send_func_t send_fn) {
    (void)handle;
    return fake_httpd::instance().set_send_override(sockfd, send_fn);
}

/**
 * @brief The link's own socket write, interposed (`-Wl,--wrap=send` on this test target).
 *
 * The link's send override does a plain BSD `::send` on the peer's fd, exactly as
 * `httpd_default_send` does — so the ONE thing a host suite has to fake for #835 is that
 * write. Descriptors the fake did not hand out are none of its business and go to the
 * real syscall, so nothing else linked into the test binary changes behaviour.
 */
extern "C" ssize_t __real_send(int fd, const void* buf, std::size_t len, int flags);
extern "C" ssize_t __wrap_send(int fd, const void* buf, std::size_t len, int flags) {
    if (!fake_httpd::instance().owns_socket(fd)) return __real_send(fd, buf, len, flags);
    return fake_httpd::instance().raw_send(fd, len);
}

/**
 * @brief The link's forced close, interposed (`-Wl,--wrap=shutdown` on the test targets).
 *
 * The one socket call the link makes that does NOT go through esp_http_server, and the
 * reason it works when the control queue does not. Descriptors the fake did not hand out
 * go to the real syscall, on the same rule as `__wrap_send`.
 */
extern "C" int __real_shutdown(int fd, int how);
extern "C" int __wrap_shutdown(int fd, int how) {
    if (!fake_httpd::instance().owns_socket(fd)) return __real_shutdown(fd, how);
    return fake_httpd::instance().raw_shutdown(fd);
}

httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t handle, int fd) {
    (void)handle;
    return fake_httpd::instance().fd_info(fd);
}
