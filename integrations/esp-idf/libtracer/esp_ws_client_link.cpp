/**
 * @file
 * @brief Implementation of @ref tr::net::esp_ws_client_link_t — the ESP-IDF
 *        `esp_transport_ws`-backed WebSocket *client* `transport_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer_esp/esp_ws_client_link.hpp"

#include <chrono>
#include <cstring>
#include <utility>

#include "esp_log.h"
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
    esp_transport_ws_set_config(ws_, &cfg);

    // esp_transport_connect performs the full RFC 6455 client handshake for ws_path_.
    const int rc = esp_transport_connect(ws_, host_.c_str(), port_, kConnectTimeoutMs);
    if (rc != 0) {
        esp_transport_close(ws_);
        return false;
    }
    connected_.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "connected ws://%s:%u%s", host_.c_str(), static_cast<unsigned>(port_),
             ws_path_.c_str());
    return true;
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
    if (frame.empty() || frame.size() > tx_buf_.size()) return;  // drop oversize/empty
    const std::lock_guard<std::mutex> lk(write_m_);
    if (!connected_.load(std::memory_order_acquire)) return;  // best-effort, like UDP
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
        connected_.store(false, std::memory_order_release);
    }
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
            // in-call by the router on this recv thread.
            if (off > 0) rx_.deliver_borrowed(std::span<const std::byte>(rx_buf_.data(), off));
            off = 0;
        } else if (off >= rx_buf_.size()) {
            // The buffer filled: the message is at least this big and may be truncated —
            // drop it rather than deliver a partial TLV (never silently truncate).
            ESP_LOGW(kTag, "inbound message exceeds %u B rx buffer — dropped",
                     static_cast<unsigned>(rx_buf_.size()));
            off = 0;
        }
        // else (!fin && off < size): keep accumulating the fragmented message.
    }
}

}  // namespace tr::net
