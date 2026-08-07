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
#include "esp_thread.hpp"
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
    // network. recv_stack==0 uses the pthread default; any other value is APPLIED —
    // this thread runs in-call delivery through the graph's on_write seam, so a node
    // that knows its delivery depth must be able to size it (#900).
    recv_thread_ = esp::spawn_thread(recv_stack, "ws_cli_rx", [this] { recv_loop(); });
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
    const int rc = esp_transport_connect(ws_, host_.c_str(), port_, kConnectTimeoutMs);
    if (rc != 0) {
        esp_transport_close(ws_);
        return false;
    }
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
    // Unread bytes of the WS FRAME currently being consumed. esp_transport_read caps
    // every read at the space we offer it and, when a frame does not fit, keeps serving
    // the rest of that SAME frame on later reads (transport_ws.c: a new header is parsed
    // only once `bytes_remaining` hits 0) — while still reporting that frame's opcode and
    // fin flag on every one of them. So `fin` alone cannot say a message ended: without
    // this counter a partial read of a big frame is indistinguishable from a complete
    // small message, which is how the tail of a dropped message got delivered (#901).
    // The frame's total length comes from esp_transport_ws_get_read_payload_len.
    std::size_t frame_left = 0;
    // Set when the message being assembled has already overflowed rx_buf_: its remaining
    // bytes are read and thrown away until the fin that ends it, so the tail is never
    // mistaken for a message of its own. This is what makes the drop-don't-truncate
    // contract actually hold (#901).
    bool discarding = false;
    // Set while a MESSAGE is open — i.e. a BINARY/TEXT frame arrived without FIN and its
    // CONT frames are still expected. This cannot be inferred from `off`: a message whose
    // first fragment carries a zero-length payload (legal per RFC 6455 §5.4, and what a
    // peer emits when it starts a message before its first chunk is ready) leaves `off`
    // at 0, so an `off == 0` test reads its CONT frames as strays and drops the whole
    // message. Assembly is a property of the FIN flags seen, not of the byte count.
    bool assembling = false;
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!connected_.load(std::memory_order_acquire)) {
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            if (!connect_once()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
            }
            continue;
        }

        const int pr = esp_transport_poll_read(ws_, kPollMs);
        if (pr < 0) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
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
                // into rx_buf_ at `off` — a zero-copy fill. While discarding, `off` is 0
                // and the whole buffer is offered as a sink for bytes we then throw away.
                n = esp_transport_read(ws_, reinterpret_cast<char*>(rx_buf_.data()) + off,
                                       static_cast<int>(rx_buf_.size() - off), kReadTimeoutMs);
            }
        }
        if (n < 0) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            continue;
        }
        // A control frame (PING/PONG/CLOSE-ack) was consumed internally — keep `off`. IDF
        // reports a ZERO-PAYLOAD data frame the same way (transport_ws.c returns 0 for
        // both, and for a poll timeout), so a message ended by an empty final fragment is
        // invisible here. That is pre-existing and symmetric: such a message never
        // delivered either, because `off` was likewise left standing.
        if (n == 0) continue;

        const ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws_);
        const bool fin = esp_transport_ws_get_fin_flag(ws_);
        if (op == WS_TRANSPORT_OPCODES_CLOSE) {
            drop();
            off = 0;
            frame_left = 0;
            discarding = false;
            assembling = false;
            continue;
        }
        if (op != WS_TRANSPORT_OPCODES_BINARY && op != WS_TRANSPORT_OPCODES_TEXT &&
            op != WS_TRANSPORT_OPCODES_CONT) {
            continue;  // a leaked control/NONE opcode: not a data frame — keep `off`
        }

        // Frame bookkeeping. A read with nothing outstanding starts a new frame, so its
        // length is the one to latch; a length that does not exceed this read (or an
        // absent one) falls back to "this read was the whole frame" — the historical
        // assumption, kept as the conservative answer.
        const std::size_t consumed = static_cast<std::size_t>(n);
        if (frame_left == 0) {
            const int plen = esp_transport_ws_get_read_payload_len(ws_);
            frame_left = plen > n ? static_cast<std::size_t>(plen) : consumed;
        }
        frame_left -= consumed < frame_left ? consumed : frame_left;
        // The MESSAGE is over only when the final frame has been consumed WHOLE.
        const bool message_done = fin && frame_left == 0;

        if (discarding) {
            // These bytes belong to the message already dropped: consume, never deliver.
            // The discard ends with that message, not with this frame.
            if (message_done) discarding = false;
            continue;
        }
        if (op != WS_TRANSPORT_OPCODES_CONT) {
            assembling = !message_done;  // a data opcode opens a message unless it ends it
        }
        if (op == WS_TRANSPORT_OPCODES_CONT && !assembling) {
            // A CONTINUATION with no assembly open — the peer's stream is out of step and
            // this is a mid-payload tail, not a message. Drop it rather than start a
            // message from the middle; the server sibling drops exactly this
            // (httpd_ws_link_t::on_data_frame).
            ESP_LOGW(kTag, "stray CONT with no message open — dropped");
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            if (!message_done) discarding = true;  // swallow the rest of that stray message
            continue;
        }

        off += consumed;
        if (message_done) {
            // A complete message that fit in the buffer: deliver borrowed, serviced
            // in-call by the router on this recv thread. `off == rx_buf_.size()` is an
            // EXACT FIT and delivers — it used to take the overflow branch (#901).
            if (off > 0) rx_.deliver_borrowed(std::span<const std::byte>(rx_buf_.data(), off));
            off = 0;
            assembling = false;
        } else if (off == rx_buf_.size()) {
            // The buffer is full and the message is NOT over: it cannot fit. Drop it
            // rather than deliver a partial TLV (never silently truncate) — and discard
            // the rest of it, or the remainder would re-accumulate from 0 and be handed
            // up as a bogus standalone frame while the peer's stream desynchronised in
            // silence (#901).
            ESP_LOGW(kTag, "inbound message exceeds %u B rx buffer — dropped",
                     static_cast<unsigned>(rx_buf_.size()));
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            off = 0;
            assembling = false;
            discarding = true;
        }
        // else (!message_done && off < size): keep accumulating the fragmented message.
    }
}

}  // namespace tr::net
