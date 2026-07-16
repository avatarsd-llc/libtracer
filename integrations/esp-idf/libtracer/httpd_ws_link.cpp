/**
 * @file
 * @brief `httpd_ws_link_t` implementation — see include/libtracer_esp/httpd_ws_link.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Chip-target-only TU (needs esp_http_server + lwIP BSD sockets), selected by the
 * component CMakeLists — never an in-source #ifdef, the same rule twai_link.cpp
 * follows. The linux virtual board keeps core's raw-socket transport_ws_server.
 */

#include "libtracer_esp/httpd_ws_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "esp_log.h"

namespace tr::net {

namespace {

constexpr const char* kTag = "httpd_ws";

/**
 * @brief httpd task stack, in bytes.
 *
 * The inbound graph request is serviced IN-CALL on this task — decode, resolve,
 * reply, and (the deep path) the whole /unit batch-apply transaction. The device
 * node measured that transaction overflowing an 8 KB stack and needing ~12 KB on
 * the raw ws recv thread (F2b, 2026-07-09); the 4 KB `esp_http_server` default is
 * far too small. Keep parity with that measured figure; httpd's own per-request
 * framing adds a little on top, so HIL should confirm this task's high-water mark
 * under a batch apply and bump it if a stack-protection reboot appears.
 */
constexpr std::size_t kHttpdTaskStack = 12288;

/**
 * @brief Upper bound on a single inbound message (one frame, or a reassembly).
 *
 * A borrowed-delivery transport heap-allocates the frame's bytes per receive, so
 * an unbounded length is a heap-exhaustion lever. Graph control-plane TLVs are far
 * smaller; a frame or reassembly past this is treated as abuse and the peer is
 * dropped (or the message discarded).
 */
constexpr std::size_t kMaxFrameBytes = 32768;

/** @brief Sockets reserved beyond the peer cap: httpd's internal working sockets
 *         plus headroom so the (cap+1)th peer is still ACCEPTED and can be refused
 *         cleanly in the handshake handler rather than held in the SYN backlog. */
constexpr std::size_t kInternalSockSlack = 3;

/** @brief `<ip>:<port>` of the far side of @p fd — the peer name (bus tag / census),
 *         byte-compatible with transport_ws_server's naming. Falls back to `fd<n>`. */
[[nodiscard]] std::string peer_name(int fd) {
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ':' + std::to_string(ntohs(addr.sin_port));
    }
    return std::string("fd") + std::to_string(fd);
}

}  // namespace

/**
 * @brief One peer slot: a single inbound WebSocket client's connection state.
 *
 * Slots are recycled in place across connections (never freed before the link), so
 * the @ref peer_endpoint_t `peer_link` hands out stays pointer-valid for the link's
 * life. Threading: `fd`/`open`/`name` are read cross-thread (peer_link /
 * enumerate_peers / a send's fd snapshot) and written by the httpd task
 * (accept/close) — all under `peers_m_`. `asm_buf` is touched only on the httpd
 * task (RX reassembly). `owner`/`endpoint` are set once at creation.
 */
struct httpd_ws_link_t::session_t {
    httpd_ws_link_t* owner = nullptr;  /**< @brief Owning link (set once, for reclaim). */
    int fd = -1;                       /**< @brief Peer socket; -1 => free slot. */
    bool open = false;                 /**< @brief True between handshake and close. */
    std::string name;                  /**< @brief `<ip>:<port>` of the peer. */
    std::vector<std::byte> asm_buf;    /**< @brief RFC 6455 fragment reassembly. */
    peer_endpoint_t endpoint;          /**< @brief The directed facade `peer_link` returns. */
};

namespace {

/** @brief One queued outbound frame: the payload is copied so it outlives the
 *         send() caller's span until the httpd task drains the work item. */
struct tx_work_t {
    httpd_handle_t handle;
    int fd;
    std::vector<std::byte> payload;
};

}  // namespace

httpd_ws_link_t::httpd_ws_link_t(std::uint16_t bind_port, std::size_t max_peers, bool peer_named)
    : port_(bind_port), max_peers_(max_peers), peer_named_(peer_named) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = bind_port;
    // A SECOND httpd instance must not share the first's control UDP port — the SPA
    // httpd (web_server.c on :80) keeps the default, so offset ours by one or
    // httpd_start fails to bind the control socket.
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    cfg.stack_size = kHttpdTaskStack;
    // Room for `max_peers` clients plus slack (see kInternalSockSlack). 0 = unbounded:
    // pick a sane finite socket budget (the shared lwIP pool is small).
    const std::size_t peers = max_peers != 0 ? max_peers : 4;
    cfg.max_open_sockets = static_cast<std::uint16_t>(peers + kInternalSockSlack);
    // Do NOT LRU-evict an existing client: at the cap we refuse the NEW peer in the
    // handshake handler, never drop a live graph peer mid-stream (transport_ws_server's
    // admission contract). lru_purge would silently sever an in-flight subscriber.
    cfg.lru_purge_enable = false;

    if (httpd_start(&handle_, &cfg) != ESP_OK) {
        handle_ = nullptr;  // ok() stays false
        return;
    }
    uri_ = "/";            // owns_httpd_ stays true; the dtor stops the server, but keep uri_
                           // coherent with the adopting path (both register the same handler).
    httpd_uri_t uri = {};  // zero-init, then set fields by name (robust to optional
    uri.uri = "/";         // ws_pre/post_handshake_cb members behind extra Kconfig)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    uri.user_ctx = this;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(handle_, &uri) != ESP_OK) {
        httpd_stop(handle_);
        handle_ = nullptr;
    }
}

httpd_ws_link_t::httpd_ws_link_t(httpd_handle_t external, const char* uri_pattern,
                                 std::size_t max_peers, bool peer_named)
    : max_peers_(max_peers), peer_named_(peer_named) {
    // Adopt an already-running server (the firmware's :80 SPA httpd): register the WS URI
    // as one more handler on it rather than standing up a second esp_http_server. We do
    // NOT own the server, so port_ is 0 (no bind of ours) and the dtor must never
    // httpd_stop it — only unregister the URI. No cfg / ctrl_port / httpd_start here: with
    // one server the control-UDP-port clash the owning ctor guards against cannot arise.
    handle_ = external;
    owns_httpd_ = false;
    port_ = 0;
    uri_ = uri_pattern;

    httpd_uri_t uri = {};    // zero-init, then set fields by name (robust to optional
    uri.uri = uri_.c_str();  // ws_pre/post_handshake_cb members behind extra Kconfig)
    uri.method = HTTP_GET;
    uri.handler = &httpd_ws_link_t::ws_handler;
    uri.user_ctx = this;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;  // httpd answers PING/PONG and tracks CLOSE
    if (httpd_register_uri_handler(external, &uri) != ESP_OK) {
        handle_ = nullptr;  // ok() stays false; do NOT httpd_stop — we do not own the server
    }
}

httpd_ws_link_t::~httpd_ws_link_t() {
    // Owning mode: stop the task first so no handler / queued work touches slots being
    // freed. On device the node leaks this object (recv path lives for the process), so
    // this only runs in a host teardown — but keep it correct. Adopted mode: only
    // unregister our WS URI and leave the caller's server running — never stop a server
    // this link did not start.
    if (handle_ != nullptr) {
        if (owns_httpd_)
            httpd_stop(handle_);
        else
            httpd_unregister_uri_handler(handle_, uri_.c_str(), HTTP_GET);
    }
    handle_ = nullptr;
}

// ---------------------------------------------------------------------------
// RX — runs on the esp_http_server task.
// ---------------------------------------------------------------------------

esp_err_t httpd_ws_link_t::ws_handler(httpd_req_t* req) {
    auto* const self = static_cast<httpd_ws_link_t*>(req->user_ctx);
    if (self == nullptr) return ESP_FAIL;
    // The opening HTTP GET Upgrade arrives as method GET (httpd has already sent the
    // 101); every subsequent data frame re-enters here with method != GET.
    if (req->method == HTTP_GET) return self->on_handshake(req);
    return self->on_data_frame(req);
}

esp_err_t httpd_ws_link_t::on_handshake(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;

    session_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        // Admission cap: refuse the new peer cleanly (ESP_FAIL => httpd closes the
        // socket). A clean refusal at the cap, not an evicted live peer.
        if (max_peers_ != 0) {
            std::size_t open_n = 0;
            for (const auto& s : slots_)
                if (s->open) ++open_n;
            if (open_n >= max_peers_) {
                ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                return ESP_FAIL;
            }
        }
        // Reuse a departed slot, else grow (push_back keeps existing slots' addresses
        // stable — endpoints handed out by peer_link stay valid).
        for (const auto& s : slots_)
            if (s->fd < 0) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            auto s = std::make_unique<session_t>();
            slot = s.get();
            slot->owner = this;
            slot->endpoint.owner_ = this;
            slot->endpoint.slot_ = slot;
            slots_.push_back(std::move(s));
        }
        slot->name = peer_name(fd);
        slot->asm_buf.clear();
        slot->fd = fd;
        slot->open = true;
    }
    // Reclaim the slot when httpd tears this session down (the free_ctx_fn fires on the
    // httpd task at close — the departure signal).
    httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);
    return ESP_OK;
}

esp_err_t httpd_ws_link_t::on_data_frame(httpd_req_t* req) {
    httpd_ws_frame_t frame = {};
    // Pass 1 (max_len 0): read the header only — fills frame.len / frame.type. The
    // payload is NOT consumed off the socket here; pass 2 below does that.
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;  // socket error => httpd closes the session
    if (frame.len > kMaxFrameBytes) return ESP_FAIL;  // abusive frame => drop the peer

    // Pass 2: ALWAYS drain the payload — even a frame type we ignore must be consumed,
    // or its bytes stay in the stream and the next recv reads them as a frame header
    // (TCP-stream misalignment). Only then decide what to do with it.
    std::vector<std::byte> payload(frame.len);
    if (frame.len != 0) {
        frame.payload = reinterpret_cast<std::uint8_t*>(payload.data());
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    // Only data frames carry a TLV (control frames are httpd's — handle_ws_control_frames
    // is off); a stray TEXT/PONG is now drained and ignored.
    if (frame.type != HTTPD_WS_TYPE_BINARY && frame.type != HTTPD_WS_TYPE_CONTINUE)
        return ESP_OK;

    const int fd = httpd_req_to_sockfd(req);
    // Resolve the slot (peer name for the bus tag + the reassembly buffer). Copy the
    // name out under the lock — the deliver below is synchronous, so a local string
    // outlives the whole in-call servicing.
    // esp_http_server responds the WS handshake INTERNALLY and (IDF v6) does NOT call the
    // URI handler for the opening GET — so on_handshake never fires. Claim the peer LAZILY
    // here, on its first data frame; an existing slot for `fd` is reused (idempotent, so the
    // on_handshake path still works on any IDF that does call the GET handler).
    session_t* slot = nullptr;
    std::string peer;
    bool newly_claimed = false;
    {
        const std::lock_guard lock(peers_m_);
        for (const auto& s : slots_)
            if (s->open && s->fd == fd) { slot = s.get(); break; }
        if (slot == nullptr) {
            // Admission cap: refuse cleanly (ESP_FAIL => httpd closes the socket).
            if (max_peers_ != 0) {
                std::size_t open_n = 0;
                for (const auto& s : slots_) if (s->open) ++open_n;
                if (open_n >= max_peers_) {
                    ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                    return ESP_FAIL;
                }
            }
            for (const auto& s : slots_)
                if (s->fd < 0) { slot = s.get(); break; }  // reuse a departed slot
            if (slot == nullptr) {
                auto s = std::make_unique<session_t>();
                slot = s.get();
                slot->owner = this;
                slot->endpoint.owner_ = this;
                slot->endpoint.slot_ = slot;
                slots_.push_back(std::move(s));
            }
            slot->name = peer_name(fd);
            slot->asm_buf.clear();
            slot->fd = fd;
            slot->open = true;
            newly_claimed = true;
        }
        peer = slot->name;
    }
    // Reclaim the slot on close — armed once, when first claimed (the free_ctx fires on the
    // httpd task at close). Outside peers_m_ so no httpd lock nests under ours.
    if (newly_claimed)
        httpd_sess_set_ctx(req->handle, fd, slot, &httpd_ws_link_t::on_session_closed);

    // Reassembly — asm_buf is httpd-task-only, so no lock. The SPA sends one whole TLV
    // per unfragmented BINARY frame (the fast path); a fragmented message chains here.
    if (frame.type == HTTPD_WS_TYPE_BINARY && frame.final && slot->asm_buf.empty()) {
        deliver(peer, payload);  // unfragmented: deliver borrowed, no extra copy
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CONTINUE && slot->asm_buf.empty())
        return ESP_OK;  // stray CONTINUE with no assembly open — drop
    if (frame.type == HTTPD_WS_TYPE_BINARY)
        slot->asm_buf.clear();  // a BINARY mid-assembly discards the stale partial
    if (slot->asm_buf.size() + payload.size() > kMaxFrameBytes) {
        slot->asm_buf.clear();
        slot->asm_buf.shrink_to_fit();
        return ESP_OK;  // reassembly would exceed the cap — drop the message
    }
    slot->asm_buf.insert(slot->asm_buf.end(), payload.begin(), payload.end());
    if (frame.final) {
        deliver(peer, slot->asm_buf);
        slot->asm_buf.clear();
        slot->asm_buf.shrink_to_fit();
    }
    return ESP_OK;
}

void httpd_ws_link_t::on_session_closed(void* ctx) {
    auto* const slot = static_cast<session_t*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->reclaim_slot(slot);
}

void httpd_ws_link_t::reclaim_slot(session_t* slot) {
    const std::lock_guard lock(peers_m_);
    slot->open = false;
    slot->fd = -1;
    slot->name.clear();
    slot->asm_buf.clear();
    slot->asm_buf.shrink_to_fit();
}

void httpd_ws_link_t::deliver(std::string_view peer, std::span<const std::byte> frame) {
    // Peer-named bus tag when the facet is on (each tab its own return route); the flat
    // point-to-point sink otherwise — matching what fwd_router_t::add_child installed.
    if (peer_named_)
        peer_rx_.deliver_borrowed(peer, frame);
    else
        rx_.deliver_borrowed(frame);
}

// ---------------------------------------------------------------------------
// TX — every send is marshalled onto the httpd task (the async-send pattern).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::queue_send(int fd, std::span<const std::byte> frame) {
    if (handle_ == nullptr || fd < 0) return;
    // Copy the payload: httpd_queue_work is asynchronous, so the caller's span is gone
    // by the time the httpd task runs tx_work. Heap, never a fixed static buffer.
    auto* const work = new (std::nothrow)
        tx_work_t{handle_, fd, std::vector<std::byte>(frame.begin(), frame.end())};
    if (work == nullptr) return;  // OOM: drop this frame (backpressure)
    if (httpd_queue_work(handle_, &httpd_ws_link_t::tx_work, work) != ESP_OK)
        delete work;  // could not enqueue — no leak
}

void httpd_ws_link_t::tx_work(void* arg) {
    auto* const work = static_cast<tx_work_t*>(arg);
    // Runs on the httpd task, sequenced with accept/close — so a peer that departed
    // between enqueue and now is a clean skip (no cross-thread send to a dead/reused fd).
    if (httpd_ws_get_fd_info(work->handle, work->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t f = {};
        f.final = true;
        f.fragmented = false;
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = reinterpret_cast<std::uint8_t*>(work->payload.data());
        f.len = work->payload.size();
        (void)httpd_ws_send_frame_async(work->handle, work->fd, &f);
    }
    delete work;
}

void httpd_ws_link_t::send(std::span<const std::byte> frame) {
    // Broadcast: one queued send per open peer (the flat point-to-point surface).
    const std::lock_guard lock(peers_m_);
    for (const auto& s : slots_)
        if (s->open) queue_send(s->fd, frame);
}

void httpd_ws_link_t::peer_endpoint_t::send(std::span<const std::byte> frame) {
    if (owner_ == nullptr || slot_ == nullptr) return;
    int fd = -1;
    {
        const std::lock_guard lock(owner_->peers_m_);
        if (!slot_->open) return;  // departed => no-op
        fd = slot_->fd;
    }
    owner_->queue_send(fd, frame);
}

// ---------------------------------------------------------------------------
// bus_link_t facet — peer enumeration / resolution (cross-thread reads).
// ---------------------------------------------------------------------------

void httpd_ws_link_t::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const auto& s : slots_)
        if (s->open && !s->name.empty()) visit(s->name);
}

transport_t* httpd_ws_link_t::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const auto& s : slots_)
        if (s->open && s->name == peer) return &s->endpoint;
    return nullptr;
}

}  // namespace tr::net
