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
#include <optional>
#include <thread>
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
    /** @brief The `headers` the last set_config carried; nullopt = the field was null. */
    std::optional<std::string> headers;
    /** @brief One entry per dial ENTERED, latched from `headers` — see dial_headers. */
    std::vector<std::optional<std::string>> dialed_headers;
    /** @brief What esp_transport_get_socket answers — see fake_ws::set_socket_fd. */
    int socket_fd = -1;

    /** @name Handle liveness and the blocking levers (#952). */
    /** @{ */
    bool tcp_live = false;      /**< @brief tcp handle created and not yet destroyed. */
    bool ws_live = false;       /**< @brief ws handle created and not yet destroyed. */
    int misuse = 0;             /**< @brief Ops on a null/destroyed handle — see handle_misuse. */
    bool fail_connect = false;  /**< @brief Every dial fails at once. */
    bool hang_connect = false;  /**< @brief Every dial blocks for its timeout, then fails. */
    int connect_timeout_ms = 0; /**< @brief The last dial's requested bound. */
    bool hold_write = false;    /**< @brief Park writers instead of completing them. */
    std::condition_variable write_cv;  /**< @brief Wakes parked writers. */
    int writers = 0;                   /**< @brief Writers parked right now. */
    int writes = 0;                    /**< @brief Writes entered since the reset. */
    int write_timeout_ms = 0;          /**< @brief The last write's requested bound. */
    std::vector<std::byte> last_write; /**< @brief The last completed write's payload. */
    /** @} */

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

/**
 * @brief With `m` held: is @p t a handle that may legally be operated on right now?
 *
 * Counts a misuse and answers false otherwise. This is the whole use-after-free
 * observable — on silicon each of these dereferences freed transport state.
 */
bool handle_usable(esp_transport_handle_t t) {
    if (t == ws_handle()) {
        if (st().ws_live) return true;
    } else if (t == tcp_handle()) {
        if (st().tcp_live) return true;
    }
    ++st().misuse;  // null, foreign, or already destroyed
    return false;
}

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
    st().tcp_live = false;
    st().ws_live = false;
    st().misuse = 0;
    st().fail_connect = false;
    st().hang_connect = false;
    st().connect_timeout_ms = 0;
    st().hold_write = false;
    st().writers = 0;
    st().writes = 0;
    st().write_timeout_ms = 0;
    st().last_write.clear();
    st().frames.clear();
    st().cur = 0;
    st().cur_off = 0;
    st().last_op = WS_TRANSPORT_OPCODES_NONE;
    st().last_fin = false;
    st().last_payload_len = 0;
    st().connected = false;
    st().connects = 0;
    st().ws_path.clear();
    st().headers.reset();
    st().dialed_headers.clear();
    st().socket_fd = -1;
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

std::vector<std::optional<std::string>> dial_headers() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().dialed_headers;
}

void set_socket_fd(int fd) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().socket_fd = fd;
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

void fail_connects(bool on) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().fail_connect = on;
}

void hang_connects(bool on) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().hang_connect = on;
}

int last_connect_timeout_ms() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().connect_timeout_ms;
}

void hold_writes(bool on) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        st().hold_write = on;
    }
    if (!on) st().write_cv.notify_all();  // the peer started accepting again
}

int writers_inside() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().writers;
}

int writes_started() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().writes;
}

int last_write_timeout_ms() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().write_timeout_ms;
}

int handle_misuse() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().misuse;
}

std::vector<std::byte> last_write_payload() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_write;
}

}  // namespace fake_ws

/** @name The `esp_transport` C surface the chip TU links against. */
/** @{ */

esp_transport_handle_t esp_transport_tcp_init(void) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().tcp_live = true;
    return tcp_handle();
}

esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent_handle) {
    const std::lock_guard<std::mutex> lk(st().m);
    if (parent_handle != tcp_handle() || !st().tcp_live) return nullptr;
    st().ws_live = true;
    return ws_handle();
}

esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t,
                                      const esp_transport_ws_config_t* config) {
    (void)t;
    const std::lock_guard<std::mutex> lk(st().m);
    if (config != nullptr && config->ws_path != nullptr) st().ws_path = config->ws_path;
    // Latched, null included: "the field was left null" and "the field was an empty
    // string" are different handshakes on the wire, and the link's contract is that no
    // headers means the byte-for-byte historical request (#959).
    st().headers = (config != nullptr && config->headers != nullptr)
                       ? std::optional<std::string>(config->headers)
                       : std::nullopt;
    return ESP_OK;
}

esp_err_t esp_transport_destroy(esp_transport_handle_t t) {
    const std::lock_guard<std::mutex> lk(st().m);
    if (!handle_usable(t)) return ESP_FAIL;
    if (t == ws_handle())
        st().ws_live = false;
    else
        st().tcp_live = false;
    return ESP_OK;
}

int esp_transport_connect(esp_transport_handle_t t, const char* host, int port, int timeout_ms) {
    (void)host;
    (void)port;
    std::unique_lock<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    ++st().connects;
    // Sampled HERE, not read back later: what dial N requested is a fact only dial N can
    // report, and what #959 left undefined was whether the first dial requested the same
    // headers as every one after it. Recorded for failed and hung dials too — what a dial
    // asked for is what it asked for, whether or not it landed.
    st().dialed_headers.push_back(st().headers);
    st().connect_timeout_ms = timeout_ms;
    if (st().hang_connect) {
        // What an unreachable peer does: spend the whole bound, then fail. Nothing
        // wakes it early — `esp_transport_connect` takes no cancellation, which is the
        // residual the link's derived dial bound is sized against.
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return -1;
    }
    if (st().fail_connect) return -1;
    st().connected = true;
    return 0;
}

int esp_transport_close(esp_transport_handle_t t) {
    const std::lock_guard<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    st().connected = false;
    return 0;
}

int esp_transport_get_socket(esp_transport_handle_t t) {
    (void)t;
    // A NUMBER, never a socket. -1 unless a test opted in with set_socket_fd, which is
    // the historical answer and keeps the suites that do not care about the option seam
    // byte-identical. Opting in does not open, read or write anything: it makes the
    // link's best-effort setsockopt block REACHABLE, and the opting-in target interposes
    // `setsockopt` so no option reaches the kernel either. Still no POSIX socket anywhere
    // in this plane (#947) — that ruling is about the transport, and this fake continues
    // to serve every byte from its own script.
    const std::lock_guard<std::mutex> lk(st().m);
    return st().socket_fd;
}

int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms) {
    std::unique_lock<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    st().cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return pending(st()); });
    return pending(st()) ? 1 : 0;
}

int esp_transport_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms) {
    (void)timeout_ms;
    const std::lock_guard<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
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
    std::unique_lock<std::mutex> lk(st().m);
    ++st().writes;  // counted on ENTRY, before the handle check: a call on a dead
                    // handle is still a call the link decided to make
    st().write_timeout_ms = timeout_ms;
    if (!handle_usable(t)) return -1;
    if (st().hold_write) {
        // A peer whose TCP window has closed. The caller is holding the link's send
        // serializer for the whole of this, which is the state the pre-#952 destructor
        // ignored. Bounded by the timeout the caller asked for, as the real write is.
        ++st().writers;
        st().write_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [] { return !st().hold_write; });
        --st().writers;
        // Re-checked on the way out, not on the way in: the real esp_transport_write
        // dereferences the handle throughout the call, so a handle destroyed while a
        // writer was parked here is a use-after-free even though it was live on entry.
        if (!handle_usable(t)) return -1;
        if (st().hold_write) return -1;  // the wait expired: a torn connection
    }
    st().last_write.assign(reinterpret_cast<const std::byte*>(buffer),
                           reinterpret_cast<const std::byte*>(buffer) + (len > 0 ? len : 0));
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
