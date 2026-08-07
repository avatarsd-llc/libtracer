/**
 * @file
 * @brief The host fake of `esp_transport_ws`'s inbound frame stream — see
 *        fake_esp_transport.hpp for what it models and why.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "fake_esp_transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#include "esp_pthread.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"

namespace {

/** @brief The whole fake's state — one connection at a time, as the link dials one. */
struct state_t {
    std::mutex m;                         /**< @brief Guards everything below. */
    std::condition_variable cv;           /**< @brief Wakes a parked `poll_read`. */
    std::vector<fake_ws::frame_t> frames; /**< @brief The scripted stream. */
    std::size_t cur = 0;                  /**< @brief Index of the frame being served. */
    std::size_t cur_off = 0;              /**< @brief Bytes of `frames[cur]` already served. */
    /**
     * @brief The frame whose header the accessors report — IDF's `frame_state`.
     *
     * Latched by a read and NOT cleared when the frame ends, exactly as
     * `transport_ws.c` leaves `frame_state` standing until the next header is parsed.
     */
    ws_transport_opcodes_t last_op = WS_TRANSPORT_OPCODES_NONE;
    bool last_fin = false;    /**< @brief That frame's FIN bit. */
    int last_payload_len = 0; /**< @brief That frame's TOTAL payload length. */

    bool connected = false; /**< @brief Between connect and close. */
    int connects = 0;       /**< @brief Dial count. */
    std::string ws_path;    /**< @brief The last dial's requested path. */

    std::size_t armed_stack = 0; /**< @brief Last arming set_cfg's stack_size. */
    std::string armed_name;      /**< @brief Its thread_name. */
    bool restored = false;       /**< @brief A zero-stack set_cfg followed it. */
    bool cfg_live = false;       /**< @brief A config is currently armed (get_cfg). */
    esp_pthread_cfg_t cfg{};     /**< @brief The armed config itself. */
};

/** @brief The single fake instance. */
state_t& st() {
    static state_t s;
    return s;
}

/** @brief The one handle value the fake hands out for the TCP transport. */
esp_transport_handle_t tcp_handle() {
    return reinterpret_cast<esp_transport_handle_t>(static_cast<std::uintptr_t>(0x7c000001));
}

/** @brief The one handle value the fake hands out for the WS transport. */
esp_transport_handle_t ws_handle() {
    return reinterpret_cast<esp_transport_handle_t>(static_cast<std::uintptr_t>(0x7c000002));
}

/** @brief True with `m` held iff an unread scripted byte (or frame) remains. */
bool pending(const state_t& s) { return s.cur < s.frames.size(); }

}  // namespace

namespace fake_ws {

frame_t make_frame(ws_transport_opcodes_t op, bool fin, std::size_t len, std::uint8_t seed) {
    frame_t f{op, fin, {}};
    f.payload.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        f.payload.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(seed + i)));
    return f;
}

void reset() {
    const std::lock_guard<std::mutex> lk(st().m);
    st().frames.clear();
    st().cur = 0;
    st().cur_off = 0;
    st().last_op = WS_TRANSPORT_OPCODES_NONE;
    st().last_fin = false;
    st().last_payload_len = 0;
    st().connected = false;
    st().connects = 0;
    st().ws_path.clear();
    st().armed_stack = 0;
    st().armed_name.clear();
    st().restored = false;
    st().cfg_live = false;
}

void push_frames(std::vector<frame_t> frames) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        for (auto& f : frames) st().frames.push_back(std::move(f));
    }
    st().cv.notify_all();
}

bool drained() {
    const std::lock_guard<std::mutex> lk(st().m);
    return !pending(st());
}

int connect_count() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().connects;
}

std::string last_ws_path() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().ws_path;
}

std::size_t armed_stack() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().armed_stack;
}

std::string armed_name() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().armed_name;
}

bool cfg_restored() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().restored;
}

}  // namespace fake_ws

/** @name The `esp_transport` C surface the chip TU links against. */
/** @{ */

esp_transport_handle_t esp_transport_tcp_init(void) { return tcp_handle(); }

esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent_handle) {
    return parent_handle == tcp_handle() ? ws_handle() : nullptr;
}

esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t,
                                      const esp_transport_ws_config_t* config) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    if (config != nullptr && config->ws_path != nullptr) st().ws_path = config->ws_path;
    return ESP_OK;
}

esp_err_t esp_transport_destroy(esp_transport_handle_t t) {
    (void)t;
    return ESP_OK;
}

int esp_transport_connect(esp_transport_handle_t t, const char* host, int port, int timeout_ms) {
    (void)t;
    (void)host;
    (void)port;
    (void)timeout_ms;
    const std::lock_guard<std::mutex> lk(st().m);
    st().connected = true;
    ++st().connects;
    return 0;
}

int esp_transport_close(esp_transport_handle_t t) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    st().connected = false;
    return 0;
}

int esp_transport_get_socket(esp_transport_handle_t t) {
    (void)t;
    return -1;  // no POSIX socket anywhere in this plane (#947)
}

int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms) {
    (void)t;
    std::unique_lock<std::mutex> lk(st().m);
    st().cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return pending(st()); });
    return pending(st()) ? 1 : 0;
}

int esp_transport_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms) {
    (void)t;
    (void)timeout_ms;
    const std::lock_guard<std::mutex> lk(st().m);
    if (!pending(st())) return 0;  // nothing queued: a timeout, exactly as ws_read reports it
    const fake_ws::frame_t& f = st().frames[st().cur];
    // The header is (re-)reported for as long as this frame is being drained.
    st().last_op = f.op;
    st().last_fin = f.fin;
    st().last_payload_len = static_cast<int>(f.payload.size());
    const std::size_t left = f.payload.size() - st().cur_off;
    const std::size_t room = len > 0 ? static_cast<std::size_t>(len) : std::size_t{0};
    const std::size_t take = std::min(left, room);
    if (take != 0) std::memcpy(buffer, f.payload.data() + st().cur_off, take);
    st().cur_off += take;
    if (st().cur_off >= f.payload.size()) {  // frame drained (a 0-byte frame lands here too)
        ++st().cur;
        st().cur_off = 0;
    }
    return static_cast<int>(take);
}

int esp_transport_write(esp_transport_handle_t t, const char* buffer, int len, int timeout_ms) {
    (void)t;
    (void)buffer;
    (void)timeout_ms;
    return len;
}

ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_op;
}

bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_fin;
}

int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_payload_len;
}

/** @} */

/** @name The `esp_pthread` stack-sizing surface (#900's observable). */
/** @{ */

esp_pthread_cfg_t esp_pthread_get_default_config(void) { return esp_pthread_cfg_t{}; }

esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* cfg) {
    if (cfg == nullptr) return ESP_FAIL;
    const std::lock_guard<std::mutex> lk(st().m);
    if (cfg->stack_size != 0) {
        st().armed_stack = cfg->stack_size;
        st().armed_name = cfg->thread_name != nullptr ? cfg->thread_name : "";
        st().restored = false;
    } else if (st().armed_stack != 0) {
        st().restored = true;  // the arming config was put back
    }
    st().cfg = *cfg;
    st().cfg_live = true;
    return ESP_OK;
}

esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* p) {
    if (p == nullptr) return ESP_FAIL;
    const std::lock_guard<std::mutex> lk(st().m);
    if (!st().cfg_live) return ESP_FAIL;  // nothing armed: leave *p untouched, as IDF does
    *p = st().cfg;
    return ESP_OK;
}

/** @} */
