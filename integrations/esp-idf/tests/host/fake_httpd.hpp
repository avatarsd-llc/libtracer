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
#include <utility>
#include <vector>

#include "esp_http_server.h"

namespace fake_httpd {

/**
 * @brief What the fake's socket write does with one buffer — the lwIP behaviours a
 *        bounded `SO_SNDTIMEO` selects between.
 */
enum class send_result_t {
    FULL,    /**< @brief The whole buffer went out: return `buf_len`. */
    TIMEOUT, /**< @brief The bound expired with ZERO bytes written: return -1. */
    SHORT    /**< @brief The bound expired MID-buffer: return the partial count. */
};

/** @brief One entry of the fake session table — httpd's `struct sock_db`, trimmed. */
struct session_t {
    void* ctx = nullptr;                    /**< @brief User context. */
    httpd_free_ctx_fn_t free_ctx = nullptr; /**< @brief Its destructor. */
    bool websocket = true;                  /**< @brief Reported by ws_get_fd_info. */
    /**
     * @brief The WebSocket route LATCHED into this session at handshake.
     *
     * httpd copies `uri->handler` / `uri->user_ctx` into the session when it answers the
     * upgrade and clears them only when the session is deleted — so an unregistered URI
     * does not stop a frame on this socket from dispatching. Modelling that is what
     * makes the post-teardown dispatch reproducible on the host at all.
     */
    esp_err_t (*ws_handler)(httpd_req_t*) = nullptr;
    void* ws_user_ctx = nullptr; /**< @brief The registered `user_ctx`, latched with it. */
    /**
     * @brief The session's socket write — httpd's `sock_db::send_fn`.
     *
     * Null means `httpd_default_send`. `httpd_sess_set_send_override` replaces it, which
     * is the only seam a link has to see the RAW return of a write and therefore the
     * only place a SHORT write can be told apart from a delivered frame.
     */
    httpd_send_func_t send_fn = nullptr;
    std::vector<send_result_t> script; /**< @brief Scripted write outcomes (see set_send_script). */
    std::size_t script_pos = 0;        /**< @brief Next entry; the last one repeats forever. */
    /**
     * @brief `shutdown(fd, SHUT_RDWR)` has been called on this socket.
     *
     * Two consequences, both lwIP's: every later write on it fails AT ONCE (no bound to
     * wait out), and the socket becomes select-readable-at-EOF — `lwip_netconn_do_shutdown`
     * fires `NETCONN_EVT_RCVPLUS` (api_msg.c) precisely so `select()` wakes. The second is
     * what lets httpd's OWN loop reap the session without any control message, which is
     * the entire reason the link uses it (see @ref server_t::run_pending).
     */
    bool shut = false;
};

/** @brief The fake server: one session table, one control queue, no sockets. */
class server_t {
   public:
    /**
     * @brief Admit a socket into the session table (the accept the fake skips) and latch
     *        the currently-registered WS route into it, as the handshake does.
     *
     * @param ctx      Optional pre-existing session context — a CO-TENANT's, when the
     *                 test is exercising descriptor reuse on the shared server.
     * @param free_fn  Its destructor, run when that session closes or changes ctx.
     */
    void open_session(int fd, void* ctx = nullptr, httpd_free_ctx_fn_t free_fn = nullptr);
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
    /**
     * @brief ONE pass of the httpd task's server loop: drain the control queue, then reap
     *        every socket a `shutdown` has made readable-at-EOF.
     *
     * @return How many things happened (items run + sessions reaped) — zero means the
     *         server has gone quiescent.
     *
     * The reap is not decoration: it is `httpd_server`'s select arm, and it is the ONLY
     * arm that does not go through the control socket. Modelling it is what lets a host
     * test tell a close that depends on the jammed queue apart from one that does not.
     */
    std::size_t run_pending();
    /** @brief Run at most ONE queued item and reap nothing — the stepper a test needs to
     *         observe the instant between "the link decided to close" and "httpd noticed".
     *  @return True if an item ran. */
    bool run_one();
    /** @brief Items currently sitting in the control queue. */
    [[nodiscard]] std::size_t queue_depth() const;
    /**
     * @brief Enqueues lwIP swallowed because the control mbox was full, ever.
     *
     * The counter a host test needs to tell "the link refused this frame itself" apart
     * from "the link handed it to a control socket that silently binned it" — two
     * outcomes that look identical from the queue's depth, and only one of which leaves
     * the httpd task free.
     */
    [[nodiscard]] std::size_t queue_drops() const;
    /**
     * @brief Cap the control queue at @p cap entries (0 = unbounded, the default).
     *
     * The cap models `CONFIG_LWIP_UDP_RECVMBOX_SIZE`: `httpd_queue_work` is a bare
     * `sendto` to a loopback UDP socket, so an enqueue past the receiver's mbox is
     * DROPPED by lwIP while `sendto` — and therefore `httpd_queue_work` — still reports
     * success (httpd_main.c, release/v5.5). See @ref set_queue_refusing for the other,
     * observable, enqueue failure.
     */
    void set_queue_capacity(std::size_t cap);

    /**
     * @brief Deliver one WebSocket frame to @p fd's LATCHED handler, as the server task
     *        would — including the `httpd_req_cleanup` epilogue.
     *
     * The route comes from the session, never from a URI table, so this reproduces the
     * dispatch that survives an unregister. Around the handler it runs the real request
     * scope: the session's ctx/free_ctx are copied into the request on entry, and on
     * return a ctx that no longer matches the socket table has its STORED destructor run
     * — the deterministic post-return `free_ctx` an in-call teardown has to survive.
     *
     * @return The handler's verdict (ESP_FAIL => the server would close the socket), or
     *         ESP_ERR_NOT_FOUND when the socket has no session / no latched route.
     */
    esp_err_t deliver_frame(int fd, std::span<const std::byte> body, bool final = true,
                            httpd_ws_type_t type = HTTPD_WS_TYPE_BINARY);

    /** @brief How many frames the link has successfully sent through the fake. */
    [[nodiscard]] std::size_t frames_sent() const;

    /**
     * @brief Script what @p fd's socket writes do, one entry per write; the LAST entry
     *        repeats once the script is exhausted (an empty script means always FULL).
     *
     * The only lever #835 needs from the socket layer: a peer whose window is full
     * (TIMEOUT), a peer whose window drains mid-frame (SHORT), and a healthy one (FULL).
     *
     * One entry is one WRITE, and one frame is TWO writes — header then payload, as
     * `httpd_ws_send_frame_async` does it (an empty frame is header-only, so one). A
     * script of `{FULL, TIMEOUT}` therefore stages a frame ANNOUNCED on the wire and then
     * abandoned, which is a different fault from `{TIMEOUT}` — a frame that never started
     * (#951). Counting writes per frame is what makes the two expressible at all.
     */
    void set_send_script(int fd, std::vector<send_result_t> script);
    /** @brief How many socket writes @p fd has taken, whatever they returned. */
    [[nodiscard]] std::size_t writes(int fd) const;

    /**
     * @brief Run @p hook between the handler's header pass and its payload pass — the
     *        instant a frame is INSIDE the handler but has claimed nothing yet.
     *
     * The only way a host test can hold a handler frame open across a concurrent
     * teardown, which is the window the destructor's handler barrier exists for.
     */
    void set_frame_hook(std::function<void()> hook);
    /** @brief Invoked by the fake's `httpd_ws_recv_frame` (payload pass). */
    void run_frame_hook();

    // --- the surface the free functions in fake_httpd.cpp forward to ---
    void* get_ctx(int fd);
    void set_ctx(int fd, void* ctx, httpd_free_ctx_fn_t free_fn);
    esp_err_t queue_work(httpd_work_fn_t work, void* arg);
    esp_err_t trigger_close(int fd);
    [[nodiscard]] httpd_ws_client_info_t fd_info(int fd);
    void note_sent();
    /**
     * @brief Refuse further enqueues with an OBSERVABLE failure — `cs_send_to_ctrl_sock`
     *        returning < 0, which `httpd_queue_work` maps to `ESP_FAIL`.
     *
     * The distinction from @ref set_queue_capacity is the whole point: this failure the
     * caller can see, the capacity drop it cannot. `httpd_sess_trigger_close` rides the
     * same socket, so it is refused too — the pre-#835-round-2 fake exempted it on the
     * theory that a full queue only DELAYS a close, and the on-silicon run refuted that
     * theory (the close never landed at all).
     */
    void set_queue_refusing(bool refusing);
    /** @brief Install a session's send override (`httpd_sess_set_send_override`). */
    esp_err_t set_send_override(int fd, httpd_send_func_t send_fn);
    /** @brief One socket write on @p fd through its session's send fn (or httpd's own
     *         default, which is the raw write plus IDF's error mapping). */
    int socket_send(int fd, const char* buf, std::size_t buf_len, int flags);
    /**
     * @brief The RAW write on @p fd — the `::send` the link's override calls, reached
     *        through the test binary's `--wrap=send` interposition.
     *
     * Returns @p buf_len (FULL), -1 with `errno == EAGAIN` (TIMEOUT), or half of
     * @p buf_len (SHORT), per @p fd's script. Sockets the fake knows nothing about are
     * not its business — see `__wrap_send`.
     */
    int raw_send(int fd, std::size_t buf_len);
    /**
     * @brief The RAW `shutdown` on @p fd, reached through `--wrap=shutdown`.
     *
     * Marks the socket shut (see @ref session_t::shut). It does NOT free the descriptor:
     * `shutdown` never does, and httpd stays the sole owner of the fd's lifetime — the
     * property that makes calling it from the link safe at all.
     */
    int raw_shutdown(int fd);
    /** @brief True while @p fd is one of the fake's sockets (the `--wrap` predicate). */
    [[nodiscard]] bool owns_socket(int fd) const;
    /** @brief True once `shutdown` has been called on @p fd. */
    [[nodiscard]] bool is_shut(int fd) const;

   private:
    /** @brief Close every socket a `shutdown` left readable-at-EOF, as httpd's select arm
     *         does; returns how many were reaped. */
    std::size_t reap_shut();
    /** @brief The one control-socket enqueue both `httpd_queue_work` and
     *         `httpd_sess_trigger_close` go through (`m_` held). */
    bool enqueue_locked(std::function<void()> item, bool* dropped);

    mutable std::mutex m_;
    std::map<int, session_t> sessions_;
    std::deque<std::function<void()>> queue_;
    std::function<void()> frame_hook_;
    std::size_t free_ctx_calls_ = 0;
    std::size_t frames_sent_ = 0;
    std::map<int, std::size_t> writes_;
    bool queue_refusing_ = false;
    std::size_t queue_cap_ = 0;   /**< @brief 0 = unbounded; see set_queue_capacity. */
    std::size_t queue_drops_ = 0; /**< @brief Enqueues lost to a full mbox. */
};

/** @brief The one fake server; its address IS the `httpd_handle_t` the link adopts. */
server_t& instance();

/**
 * @brief The currently-registered WS route: what a handshake would latch into a session.
 *
 * `httpd_unregister_uri_handler` clears it, exactly as the real one drops the route —
 * but sessions that latched it earlier keep dispatching, which is the whole point.
 */
struct route_t {
    esp_err_t (*handler)(httpd_req_t*) = nullptr; /**< @brief The URI handler. */
    void* user_ctx = nullptr;                     /**< @brief Its registered `user_ctx`. */
};

/** @brief The route a handshake would latch right now (null handler => unregistered). */
route_t registered_route();

/** @brief Record (or, with a null handler, drop) the route — called by the fake C API. */
void set_registered_route(route_t route);

}  // namespace fake_httpd
