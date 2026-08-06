/**
 * @file
 * @brief Implementation of @ref tr::net::esp_ws_client_link_t — the ESP-IDF
 *        `esp_transport_ws`-backed WebSocket *client* `transport_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer_esp/esp_ws_client_link.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <chrono>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"

namespace tr::net {

namespace {
/** @brief Log tag for this link. */
constexpr const char* kTag = "ws_client_link";
/** @brief Connect timeout for one dial attempt (ms). */
constexpr int kConnectTimeoutMs = 5000;
/** @brief Read timeout after a positive poll (ms) — data is already buffered, so short. */
constexpr int kReadTimeoutMs = 100;
/** @brief Write timeout (ms) — generous vs the read timeout so momentary TX congestion
 *  does not spuriously tear the connection down (a 100 ms write timeout thrashes). */
constexpr int kWriteTimeoutMs = 4000;
/** @brief Poll wait per recv-loop turn (ms) — bounds how fast a stop is observed. */
constexpr int kPollMs = 200;
/** @brief Backoff before re-dialing after a failed/lost connection (ms). */
constexpr int kReconnectBackoffMs = 1500;
}  // namespace

esp_ws_client_link_t::esp_ws_client_link_t(std::string host, std::uint16_t port,
                                           std::string ws_path, std::size_t rx_bytes,
                                           std::size_t tx_bytes, std::size_t recv_stack)
    : host_(std::move(host)),
      port_(port),
      ws_path_(std::move(ws_path)),
      rx_buf_(rx_bytes),
      tx_buf_(tx_bytes) {
    // The recv thread owns dialing + the read loop; the ctor never blocks on the
    // network. recv_stack==0 uses the pthread default (the node sizes it for the
    // in-call delivery depth through the graph's on_write seam).
    (void)recv_stack;  // std::thread uses the platform pthread default stack
    recv_thread_ = std::thread([this] { recv_loop(); });
}

esp_ws_client_link_t::~esp_ws_client_link_t() {
    stop_.store(true, std::memory_order_relaxed);
    if (recv_thread_.joinable()) recv_thread_.join();
    // The recv thread has stopped, so no concurrent handle access remains.
    // esp_transport_ws_init(parent) does NOT take ownership of the parent, so both
    // handles are destroyed here (ws first, then tcp) — mirrors IDF's own teardown.
    if (ws_ != nullptr) {
        esp_transport_close(ws_);
        esp_transport_destroy(ws_);
        ws_ = nullptr;
    }
    if (tcp_ != nullptr) {
        esp_transport_destroy(tcp_);
        tcp_ = nullptr;
    }
}

bool esp_ws_client_link_t::connect_once() {
    // Build a FRESH tcp+ws transport pair for every dial. Re-using a closed
    // esp_transport_ws handle would inherit its internal frame_state (bytes_remaining
    // etc.) — ws_connect() does not reset it, only ws_init() zero-allocates it — so a
    // reconnect mid-fragment would mis-parse the first frame of the new connection.
    // Rebuilding is the only way to guarantee a clean frame state.
    if (ws_ != nullptr) {
        esp_transport_close(ws_);
        esp_transport_destroy(ws_);
        ws_ = nullptr;
    }
    if (tcp_ != nullptr) {
        esp_transport_destroy(tcp_);
        tcp_ = nullptr;
    }
    tcp_ = esp_transport_tcp_init();
    if (tcp_ == nullptr) return false;
    ws_ = esp_transport_ws_init(tcp_);
    if (ws_ == nullptr) return false;
    esp_transport_ws_config_t cfg = {};
    cfg.ws_path = ws_path_.c_str();
    cfg.propagate_control_frames = false;  // esp_transport_ws answers PING/CLOSE itself
    // Optional handshake auth: extra header lines a peer's admission hook can gate on (a
    // b2b dial carries no browser cookie). esp_transport_ws appends them verbatim; empty
    // leaves the field null so the handshake is byte-for-byte the historical one.
    if (!handshake_headers_.empty()) cfg.headers = handshake_headers_.c_str();
    esp_transport_ws_set_config(ws_, &cfg);

    // esp_transport_connect performs the full RFC 6455 client handshake for ws_path_.
    // Timed: TCP connect + the opening exchange is an UPPER BOUND on ~2x RTT, and it is
    // the only latency fact this link can offer without an active probe (see stats_t).
    const std::int64_t dial_t0 = esp_timer_get_time();
    const int rc = esp_transport_connect(ws_, host_.c_str(), port_, kConnectTimeoutMs);
    if (rc != 0) {
        esp_transport_close(ws_);
        return false;
    }
    const std::int64_t dial_us = esp_timer_get_time() - dial_t0;
    // Disable Nagle on the freshly connected socket, symmetric with the server side
    // (httpd_ws_link_t::bound_socket): a board-to-board dial carries the same small,
    // latency-sensitive TLV frames whose replies the peer awaits, so delayed-ACK +
    // Nagle would add tens of ms per round-trip. esp_transport exposes the underlying
    // fd; best-effort, so a transport that hides it (rc < 0) just keeps Nagle for
    // this link rather than failing the dial.
    const int fd = esp_transport_get_socket(tcp_);
    if (fd >= 0) {
        const int nodelay = 1;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0)
            ESP_LOGW(kTag, "TCP_NODELAY not applied fd=%d", fd);
    }
    // The connection edge, and the only place the link can observe one: `drop()`
    // deliberately stays silent (see below), so this counter IS the reconnect signal.
    // The traffic counters are per-CONNECTION, so they reset here; `last_rx_us` goes
    // back to "never" rather than carrying the previous session's staleness forward.
    {
        const std::lock_guard<std::mutex> lk(st_m_);
        st_ = {};
        st_.connected_at_us = esp_timer_get_time();
        connect_ms_ = static_cast<std::uint32_t>(dial_us / 1000);
        ++reconnects_;
    }
    connected_.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "connected ws://%s:%u%s", host_.c_str(), static_cast<unsigned>(port_),
             ws_path_.c_str());
    return true;
}

esp_ws_client_link_t::stats_t esp_ws_client_link_t::stats() const {
    stats_t out;
    {
        // st_m_, NOT write_m_. write_m_ is held across esp_transport_write for up to
        // kWriteTimeoutMs (4 s) on a stalled socket, so a snapshot taken under it
        // would drag any caller — the embedder's periodic publisher, which may hold
        // its own lock across this call — into that wait. This mutex is only ever
        // held for a counter bump or this copy, so the snapshot is bounded-brief.
        const std::lock_guard<std::mutex> lk(st_m_);
        out.c = st_;
        out.reconnects = reconnects_;
        out.connect_ms = connect_ms_;
    }
    // Read OUTSIDE the mutex: `connected_` is atomic and `drop()` clears it while
    // holding write_m_, so taking it inside would say nothing extra and reading it
    // here keeps the hold to the struct copy.
    out.up = connected_.load(std::memory_order_acquire);
    return out;
}

void esp_ws_client_link_t::drop() {
    // Close under the syscall serializer so an in-flight send() cannot touch a
    // half-closed handle, and mark down so the recv loop re-dials (which rebuilds
    // the transport pair — see connect_once). NOT firing notify_down: a transient
    // drop keeps the producer's subscriber edges so deliveries resume transparently
    // on reconnect ("configured but idle, resumes on reconnect"), instead of evicting
    // bindings on every network blip.
    const std::lock_guard<std::mutex> lk(write_m_);
    if (ws_ != nullptr) esp_transport_close(ws_);
    connected_.store(false, std::memory_order_release);
}

void esp_ws_client_link_t::send(std::span<const std::byte> frame) {
    // Counted under st_m_, never under write_m_ — write_m_ is held across the
    // transport write below for up to kWriteTimeoutMs, and a counter that rode it
    // would make every stats() snapshot inherit that wait. st_m_ is only ever taken
    // for these bumps, so it is uncontended and syscall-free. Nesting order is always
    // write_m_ -> st_m_ and never the reverse, which keeps it acyclic.
    const auto bump = [this](auto fn) {
        const std::lock_guard<std::mutex> lk(st_m_);
        fn();
    };
    // Taken FIRST so the two drop paths below are counted: both are real losses toward
    // this peer, and the second one ("peer is down, the push vanishes") is the one
    // that used to be completely invisible. Neither check does any work, so hoisting
    // them under the serializer costs nothing.
    const std::lock_guard<std::mutex> lk(write_m_);
    if (frame.empty() || frame.size() > tx_buf_.size()) {  // drop oversize/empty
        bump([this] { ++st_.tx_drops; });
        return;
    }
    if (!connected_.load(std::memory_order_acquire)) {  // best-effort, like UDP
        bump([this] { ++st_.tx_drops; });
        return;
    }
    // Copy into the reusable scratch: esp_transport_write masks IN-PLACE and unmasks
    // back (RFC 6455 client rule), but a delivered frame may be shared with the
    // concurrent server link reading the same bytes, so the caller's bytes must not be
    // transiently mutated — hence the copy onto our private scratch.
    std::memcpy(tx_buf_.data(), frame.data(), frame.size());
    const int n = esp_transport_write(ws_, reinterpret_cast<char*>(tx_buf_.data()),
                                      static_cast<int>(frame.size()), kWriteTimeoutMs);
    if (n < 0 || n < static_cast<int>(frame.size())) {
        // Error or short write (a partial WS frame would desync the peer) — tear the
        // connection down so the recv loop rebuilds it; the frame is best-effort-lost.
        bump([this] { ++st_.tx_drops; });
        connected_.store(false, std::memory_order_release);
        return;
    }
    bump([this, n = frame.size()] {
        ++st_.tx_frames;
        st_.tx_bytes += static_cast<std::uint32_t>(n);
    });
}

void esp_ws_client_link_t::recv_loop() {
    // `off` is the in-progress message accumulator; it PERSISTS across poll turns so a
    // fragmented message (spread over reads/polls, possibly interleaved with control
    // frames) reassembles correctly. Reset only on delivery, overflow, or a drop.
    std::size_t off = 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!connected_.load(std::memory_order_acquire)) {
            off = 0;
            if (!connect_once()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
            }
            continue;
        }

        const int pr = esp_transport_poll_read(ws_, kPollMs);
        if (pr < 0) {
            drop();
            off = 0;
            continue;
        }
        if (pr == 0) continue;  // no data this turn; re-check stop_

        int n = 0;
        {
            const std::lock_guard<std::mutex> lk(write_m_);
            if (!connected_.load(std::memory_order_acquire)) {
                n = -1;
            } else {
                // Server→client frames are unmasked, so this reads the payload directly
                // into rx_buf_ at `off` — a zero-copy fill.
                n = esp_transport_read(ws_, reinterpret_cast<char*>(rx_buf_.data()) + off,
                                       static_cast<int>(rx_buf_.size() - off), kReadTimeoutMs);
            }
        }
        if (n < 0) {
            drop();
            off = 0;
            continue;
        }
        if (n == 0) continue;  // a control frame (PING/PONG/CLOSE-ack) was consumed — keep `off`

        const ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws_);
        const bool fin = esp_transport_ws_get_fin_flag(ws_);
        if (op == WS_TRANSPORT_OPCODES_CLOSE) {
            drop();
            off = 0;
            continue;
        }
        if (op != WS_TRANSPORT_OPCODES_BINARY && op != WS_TRANSPORT_OPCODES_TEXT &&
            op != WS_TRANSPORT_OPCODES_CONT) {
            continue;  // a leaked control/NONE opcode: not a data frame — keep `off`
        }

        off += static_cast<std::size_t>(n);
        if (fin && off < rx_buf_.size()) {
            // A complete message that fit in the buffer: deliver borrowed, serviced
            // in-call by the router on this recv thread. Count it BEFORE the delivery,
            // under st_m_ and NOT write_m_: write_m_ is held across a transport write
            // for up to kWriteTimeoutMs, so counting under it would stall every
            // inbound graph op behind one slow outbound frame. No lock at all is held
            // across the delivery itself — the router runs the app in-call and the app
            // may call back into send() on this very stack.
            if (off > 0) {
                {
                    const std::lock_guard<std::mutex> lk(st_m_);
                    ++st_.rx_frames;
                    st_.rx_bytes += static_cast<std::uint32_t>(off);
                    st_.last_rx_us = esp_timer_get_time();
                }
                rx_.deliver_borrowed(std::span<const std::byte>(rx_buf_.data(), off));
            }
            off = 0;
        } else if (off >= rx_buf_.size()) {
            // The buffer filled: the message is at least this big and may be truncated —
            // drop it rather than deliver a partial TLV (never silently truncate).
            {
                const std::lock_guard<std::mutex> lk(st_m_);
                ++st_.rx_drops;
            }
            ESP_LOGW(kTag, "inbound message exceeds %u B rx buffer — dropped",
                     static_cast<unsigned>(rx_buf_.size()));
            off = 0;
        }
        // else (!fin && off < size): keep accumulating the fragmented message.
    }
}

}  // namespace tr::net
