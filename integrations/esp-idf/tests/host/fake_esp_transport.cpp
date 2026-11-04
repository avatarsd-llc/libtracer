/**
 * @file
 * @brief The host fake of `tcp_transport`'s byte stream, as a WebSocket server's end of
 *        it — see fake_esp_transport.hpp for what it models and why.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "fake_esp_transport.hpp"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "libtracer/ws.hpp"

namespace {

/** @brief The whole fake's state — one connection at a time, as the link dials one. */
struct state_t {
    std::mutex m;                /**< @brief Guards everything below. */
    std::condition_variable cv;  /**< @brief Wakes a parked `poll_read`. */
    std::vector<std::byte> wire; /**< @brief The scripted frames, encoded as a server sends. */
    std::size_t wire_off = 0;    /**< @brief Bytes of `wire` already served. */
    /** @brief The connection the scripted frames go to: the one upgraded last. */
    esp_transport_handle_t stream = nullptr;
    bool reject_upgrade = false; /**< @brief Answer handshakes with a wrong accept. */
    /** @brief The answer's header lines between the status line's CRLF and the accept
     *         line; `std::nullopt` = the default 101 (see fake_ws::answer_upgrades_with). */
    std::optional<std::pair<std::string, std::string>> answer;
    int handshakes = 0; /**< @brief Handshake requests received. */

    int connects = 0;    /**< @brief Dial count. */
    std::string ws_path; /**< @brief The last handshake's requested path. */
    /** @brief One entry per handshake request — see dial_headers. */
    std::vector<std::optional<std::string>> dialed_headers;
    /** @brief What esp_transport_get_socket answers — see fake_ws::set_socket_fd. */
    int socket_fd = -1;

    /** @name Handle liveness and the blocking levers (#952, #1058). */
    /** @{ */
    /** @brief One connection's own stream state: its handshake and its response. */
    struct conn_t {
        bool upgraded = false;       /**< @brief Its handshake request has been answered. */
        std::vector<std::byte> resp; /**< @brief The `101` queued for it. */
        std::size_t resp_off = 0;    /**< @brief Bytes of `resp` already served. */
    };
    /**
     * @brief Every handle created and not yet destroyed, with its connection's state.
     *
     * A REGISTRY, because #1058 puts two connections on the fake at once: an orphaned dial
     * still holding its own while the link that started it is gone and a fresh link has
     * dialed its own. A single-connection model would score the orphan's release as a
     * misuse against the live link's handle, which is the one counter the whole suite rests
     * on.
     */
    std::map<esp_transport_handle_t, conn_t> live;
    /** @brief Next handle value to hand out — distinct per handle, monotonic. */
    std::uintptr_t next_handle = 0x7c000001;
    int misuse = 0;            /**< @brief Ops on a null/destroyed handle — see handle_misuse. */
    int closes = 0;            /**< @brief Closes that reached a live handle. */
    bool fail_connect = false; /**< @brief Every dial fails at once. */
    bool hang_connect = false; /**< @brief Every dial blocks for its timeout, then fails. */
    std::condition_variable connect_cv; /**< @brief Wakes dials parked by `hang_connect`. */
    int dialers = 0;                    /**< @brief Dials parked right now. */
    bool connect_released = false;      /**< @brief A release was issued — see
                                         *          fake_ws::release_connects. */
    bool connect_succeed = false;       /**< @brief What that release makes them report. */
    int connect_timeout_ms = 0;         /**< @brief The last dial's requested bound. */
    bool hold_write = false;            /**< @brief Park writers instead of completing them. */
    bool pin_write = false;             /**< @brief Park writers past their own timeout,
                                         *          until fake_ws::pin_writes(false). */
    std::condition_variable write_cv;   /**< @brief Wakes parked writers. */
    int writers = 0;                    /**< @brief Writers parked right now. */
    int writes = 0;                     /**< @brief Writes entered since the reset. */
    int write_timeout_ms = 0;           /**< @brief The last write's requested bound. */
    std::vector<std::byte> last_write;  /**< @brief The last completed write's payload. */
    int last_write_op = -1;             /**< @brief Its WS opcode; -1 = none yet. */
    bool last_write_masked = false;     /**< @brief Its MASK bit. */
    int lwip_calls = 0;                /**< @brief lwIP calls the writes made — see lwip_calls. */
    std::size_t wire_bytes = 0;        /**< @brief Bytes handed to lwIP's send. */
    bool hold_read = false;            /**< @brief Park readers instead of serving them. */
    std::condition_variable read_cv;   /**< @brief Wakes parked readers. */
    int readers = 0;                   /**< @brief Readers parked right now. */
    bool park_socket = false;          /**< @brief Park `esp_transport_get_socket`. */
    bool socket_parked = false;        /**< @brief A caller is parked there. */
    std::condition_variable socket_cv; /**< @brief Wakes it. */
    /** @} */

    std::size_t dial_stack = 0; /**< @brief The first dialing thread's stack size. */
};

/** @brief The single fake instance. */
state_t& st() {
    static state_t s;
    return s;
}

/** @brief With `m` held: mint a fresh live handle. */
esp_transport_handle_t mint_handle() {
    const auto h = reinterpret_cast<esp_transport_handle_t>(st().next_handle++);
    st().live.emplace(h, state_t::conn_t{});
    return h;
}

/** @brief With `m` held: true iff an unread scripted frame byte remains. */
bool frames_pending(const state_t& s) { return s.wire_off < s.wire.size(); }

/** @brief With `m` held: true iff connection @p t has a byte to serve — its `101`, then,
 *         once it is the stream, the scripted frames. */
bool pending(esp_transport_handle_t t) {
    const auto it = st().live.find(t);
    if (it == st().live.end()) return false;
    const state_t::conn_t& c = it->second;
    return c.resp_off < c.resp.size() || (c.upgraded && st().stream == t && frames_pending(st()));
}

/** @brief Append @p f to @p out as a server frame: unmasked, with a 2/4/10-byte header. */
void encode_frame(const fake_ws::frame_t& f, std::vector<std::byte>& out) {
    const std::size_t n = f.payload.size();
    out.push_back(static_cast<std::byte>((f.fin ? 0x80U : 0U) | static_cast<unsigned>(f.op)));
    if (n < 126) {
        out.push_back(static_cast<std::byte>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(std::byte{126});
        for (int i = 1; i >= 0; --i) out.push_back(static_cast<std::byte>((n >> (8 * i)) & 0xFF));
    } else {
        out.push_back(std::byte{127});
        for (int i = 7; i >= 0; --i)
            out.push_back(
                static_cast<std::byte>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    out.insert(out.end(), f.payload.begin(), f.payload.end());
}

/**
 * @brief With `m` held: answer the handshake request @p req written on @p t — record what
 *        it asked for and queue the `101` on that connection. @return whether it parsed.
 */
bool answer_handshake(esp_transport_handle_t t, std::string_view req) {
    if (!req.starts_with("GET ") || !req.ends_with("\r\n\r\n")) return false;
    const std::size_t path_end = req.find(' ', 4);
    if (path_end == std::string_view::npos) return false;
    ++st().handshakes;
    st().ws_path = std::string(req.substr(4, path_end - 4));
    // The extra lines: every header line that is not one of the standard Upgrade lines.
    static constexpr std::string_view kStandard[] = {
        "host:", "upgrade:", "connection:", "sec-websocket-key:", "sec-websocket-version:"};
    std::string extra;
    for (std::size_t at = req.find("\r\n") + 2; at < req.size();) {
        const std::size_t eol = req.find("\r\n", at);
        const std::string_view line = req.substr(at, eol - at);
        at = eol + 2;
        if (line.empty()) break;
        std::string lower(line);
        for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        bool standard = false;
        for (const std::string_view name : kStandard)
            standard = standard || lower.starts_with(name);
        if (!standard) extra.append(line).append("\r\n");
    }
    st().dialed_headers.push_back(extra.empty() ? std::nullopt
                                                : std::optional<std::string>(std::move(extra)));
    const std::string_view key = tr::net::ws::header_value(req, "sec-websocket-key");
    const std::string accept(st().reject_upgrade ? std::string_view("AAAAAAAAAAAAAAAAAAAAAAAAAAA=")
                                                 : tr::net::ws::accept_key(key).view());
    const std::string resp = st().answer
                                 ? st().answer->first + "\r\n" + st().answer->second +
                                       "Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
                                 : "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                       accept + "\r\n\r\n";
    state_t::conn_t& c = st().live[t];
    c.resp.assign(reinterpret_cast<const std::byte*>(resp.data()),
                  reinterpret_cast<const std::byte*>(resp.data()) + resp.size());
    c.resp_off = 0;
    c.upgraded = true;
    st().stream = t;
    return true;
}

/**
 * @brief With `m` held: is @p t a handle that may legally be operated on right now?
 *
 * Counts a misuse and answers false otherwise. This is the whole use-after-free
 * observable — on silicon each of these dereferences freed transport state. Per HANDLE
 * since #1058: two transport pairs coexist while an orphaned dial resolves under a link
 * that is already gone, and identity is what keeps the orphan's release from being
 * charged against the live link's pair.
 */
bool handle_usable(esp_transport_handle_t t) {
    if (st().live.find(t) != st().live.end()) return true;
    ++st().misuse;  // null, foreign, or already destroyed
    return false;
}

}  // namespace

namespace fake_ws {

frame_t make_frame(tr::net::ws::opcode_t op, bool fin, std::size_t len, std::uint8_t seed) {
    frame_t f{op, fin, {}};
    f.payload.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        f.payload.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(seed + i)));
    return f;
}

void reset() {
    const std::lock_guard<std::mutex> lk(st().m);
    st().live.clear();
    st().misuse = 0;
    st().closes = 0;
    st().fail_connect = false;
    st().hang_connect = false;
    st().connect_released = false;
    st().connect_succeed = false;
    st().dialers = 0;
    st().connect_timeout_ms = 0;
    st().hold_write = false;
    st().pin_write = false;
    st().writers = 0;
    st().writes = 0;
    st().write_timeout_ms = 0;
    st().last_write.clear();
    st().last_write_op = -1;
    st().last_write_masked = false;
    st().lwip_calls = 0;
    st().wire_bytes = 0;
    st().hold_read = false;
    st().readers = 0;
    st().park_socket = false;
    st().socket_parked = false;
    st().wire.clear();
    st().wire_off = 0;
    st().stream = nullptr;
    st().reject_upgrade = false;
    st().answer.reset();
    st().handshakes = 0;
    st().connects = 0;
    st().ws_path.clear();
    st().dialed_headers.clear();
    st().socket_fd = -1;
    st().dial_stack = 0;
}

void push_frames(std::vector<frame_t> frames) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        for (const frame_t& f : frames) encode_frame(f, st().wire);
    }
    st().cv.notify_all();
}

bool drained() {
    const std::lock_guard<std::mutex> lk(st().m);
    return !frames_pending(st());
}

void reject_upgrades(bool on) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().reject_upgrade = on;
}

void answer_upgrades_with(std::optional<std::pair<std::string, std::string>> answer) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().answer = std::move(answer);
}

int handshakes() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().handshakes;
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

std::size_t dial_stack() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().dial_stack;
}

void fail_connects(bool on) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().fail_connect = on;
}

void hang_connects(bool on) {
    const std::lock_guard<std::mutex> lk(st().m);
    st().hang_connect = on;
}

void release_connects(bool succeed) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        st().connect_released = true;
        st().connect_succeed = succeed;
    }
    st().connect_cv.notify_all();
}

int dialers_inside() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().dialers;
}

int live_handles() {
    const std::lock_guard<std::mutex> lk(st().m);
    return static_cast<int>(st().live.size());
}

bool wait_drained(std::chrono::milliseconds limit) {
    const auto drained_now = [] {
        const std::lock_guard<std::mutex> lk(st().m);
        return st().live.empty() && st().dialers == 0;
    };
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (drained_now()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return drained_now();
}

int close_count() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().closes;
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

void pin_writes(bool on) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        st().pin_write = on;
    }
    if (!on) st().write_cv.notify_all();
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

int last_write_op() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_write_op;
}

bool last_write_masked() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().last_write_masked;
}

int lwip_calls() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().lwip_calls;
}

std::size_t wire_bytes() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().wire_bytes;
}

void hold_reads(bool on) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        st().hold_read = on;
    }
    if (!on) st().read_cv.notify_all();
}

int readers_inside() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().readers;
}

void park_get_socket(bool on) {
    {
        const std::lock_guard<std::mutex> lk(st().m);
        st().park_socket = on;
    }
    if (!on) st().socket_cv.notify_all();
}

bool get_socket_parked() {
    const std::lock_guard<std::mutex> lk(st().m);
    return st().socket_parked;
}

}  // namespace fake_ws

/** @name The `esp_transport` C surface the chip TU links against. */
/** @{ */

esp_transport_handle_t esp_transport_tcp_init(void) {
    const std::lock_guard<std::mutex> lk(st().m);
    return mint_handle();
}

esp_err_t esp_transport_destroy(esp_transport_handle_t t) {
    const std::lock_guard<std::mutex> lk(st().m);
    if (!handle_usable(t)) return ESP_FAIL;
    st().live.erase(t);
    if (st().stream == t) st().stream = nullptr;
    return ESP_OK;
}

int esp_transport_connect(esp_transport_handle_t t, const char* host, int port, int timeout_ms) {
    (void)host;
    (void)port;
    std::unique_lock<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    ++st().connects;
    if (st().dial_stack == 0) {
        pthread_attr_t attr;
        if (::pthread_getattr_np(::pthread_self(), &attr) == 0) {
            (void)::pthread_attr_getstacksize(&attr, &st().dial_stack);
            ::pthread_attr_destroy(&attr);
        }
    }
    st().connect_timeout_ms = timeout_ms;
    if (st().hang_connect) {
        // What an unreachable peer does: spend the whole bound, then fail. Nothing the
        // LINK can do wakes it early — `esp_transport_connect` takes no cancellation,
        // which is the residual #1058 is about; the only thing that resolves it is the
        // peer answering, which `fake_ws::release_connects` stands in for. Bounded by the
        // timeout the caller asked for, exactly as the parked write above is.
        ++st().dialers;
        st().connect_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [] { return st().connect_released; });
        --st().dialers;
        // Re-checked on the way out, exactly as the parked write is: the real
        // `esp_transport_connect` dereferences the handle for the whole call, so a pair
        // destroyed while a dial is parked in it is a use-after-free even though it was
        // live on entry. That is the failure mode a "cancel the dial by destroying its
        // transport" fix would have, and this is what would count it.
        if (!handle_usable(t)) return -1;
        if (!st().connect_released) return -1;  // the bound expired: an unreachable peer
        if (!st().connect_succeed) return -1;
        return 0;
    }
    if (st().fail_connect) return -1;
    return 0;
}

int esp_transport_close(esp_transport_handle_t t) {
    const std::lock_guard<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    ++st().closes;
    state_t::conn_t& c = st().live[t];
    // The end of this connection's stream: whatever was still queued for it is gone with it.
    if (st().stream == t) {
        st().wire.clear();
        st().wire_off = 0;
        st().stream = nullptr;
    }
    c = state_t::conn_t{};
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
    std::unique_lock<std::mutex> lk(st().m);
    if (st().park_socket) {
        st().socket_parked = true;
        st().socket_cv.wait(lk, [] { return !st().park_socket; });
        st().socket_parked = false;
    }
    return st().socket_fd;
}

int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms) {
    std::unique_lock<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    st().cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [t] { return pending(t); });
    return pending(t) ? 1 : 0;
}

int esp_transport_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms) {
    (void)timeout_ms;
    std::unique_lock<std::mutex> lk(st().m);
    if (!handle_usable(t)) return -1;
    if (st().hold_read) {
        // A read parked inside the transport until the test releases it — NOT bounded by
        // the caller's timeout, so a sender that waits on this reader waits until the test
        // gives up, which is what makes "a read holds no send" a measurement (#2265).
        ++st().readers;
        st().read_cv.wait(lk, [] { return !st().hold_read; });
        --st().readers;
        if (!handle_usable(t)) return -1;
    }
    // `tcp_read`: whatever is queued, up to the room offered, across frame boundaries — the
    // connection's `101` first, then (once it is the stream) the scripted frames.
    state_t::conn_t& c = st().live[t];
    const std::size_t room = len > 0 ? static_cast<std::size_t>(len) : std::size_t{0};
    std::size_t got = 0;
    const std::size_t from_resp = std::min(room, c.resp.size() - c.resp_off);
    if (from_resp != 0) {  // an empty vector's data() may be null: memcpy(_, nullptr, 0) is UB
        std::memcpy(buffer, c.resp.data() + c.resp_off, from_resp);
    }
    c.resp_off += from_resp;
    got += from_resp;
    if (c.upgraded && st().stream == t) {
        const std::size_t from_wire = std::min(room - got, st().wire.size() - st().wire_off);
        if (from_wire != 0) {
            std::memcpy(buffer + got, st().wire.data() + st().wire_off, from_wire);
        }
        st().wire_off += from_wire;
        got += from_wire;
    }
    return static_cast<int>(got);  // 0 = nothing queued: `tcp_read`'s timeout
}

int esp_transport_write(esp_transport_handle_t t, const char* buffer, int len, int timeout_ms) {
    std::unique_lock<std::mutex> lk(st().m);
    if (handle_usable(t) && !st().live[t].upgraded) {
        // A connection's first write is its opening-handshake request: answered, and not a
        // frame, so no write counter or lever below applies to it.
        return answer_handshake(
                   t, std::string_view(buffer, static_cast<std::size_t>(len > 0 ? len : 0)))
                   ? len
                   : -1;
    }
    ++st().writes;  // counted on ENTRY, before the handle check: a call on a dead
                    // handle is still a call the link decided to make
    st().write_timeout_ms = timeout_ms;
    if (!handle_usable(t)) return -1;
    if (st().pin_write) {
        // Parked until the TEST lets go, whatever the caller's timeout: the state a test
        // checks against is then held, not raced against the link's own write bound. The
        // release stands for that bound expiring, so the write fails as it would have.
        ++st().writers;
        st().write_cv.wait(lk, [] { return !st().pin_write; });
        --st().writers;
        (void)handle_usable(t);  // a handle destroyed under the parked writer is misuse
        return -1;
    }
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
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(buffer),
                                           static_cast<std::size_t>(len > 0 ? len : 0));
    // A frame write is `tcp_write` (transport_ssl.c): a poll, then one send.
    // The bytes are a whole client frame; decode it as the peer would, unmasking a copy.
    st().lwip_calls += 2;
    st().wire_bytes += bytes.size();
    const tr::net::ws::header_t h = tr::net::ws::detail::decode_header(
        bytes, /*fail_on_violation=*/false, tr::net::ws::kNoPayloadCap);
    if (h.size + h.len != bytes.size()) {
        ++st().misuse;  // not exactly one frame: the peer's stream would desync
        return -1;
    }
    st().last_write.assign(bytes.begin() + static_cast<std::ptrdiff_t>(h.size), bytes.end());
    if (h.masked) tr::net::ws::detail::xor_mask(st().last_write, h.mask_key);
    st().last_write_op = static_cast<int>(h.op);
    st().last_write_masked = h.masked;
    return len;
}

/** @} */
