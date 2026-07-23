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
#include <memory>
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

/**
 * @brief Consecutive TX-enqueue drops that mark a session broken (then close it).
 *
 * A single failed enqueue (httpd's control queue momentarily full, or the work-item
 * copy failing to allocate) is transient backpressure — dropping that one frame is
 * the lean response, and the next successful enqueue resets the count. But a session
 * whose enqueues keep failing with no success in between is not riding out a burst:
 * its peer is silently missing frames while the socket looks open. Three in a row
 * distinguishes the two — one drop is noise, two can straddle a burst, three
 * consecutive means the drain isn't keeping up at all. This is a brokenness
 * detector, not a tunable, so it is a named constant and NOT a config knob.
 */
constexpr std::uint8_t kMaxConsecutiveTxDrops = 3;

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

/**
 * @brief Nothrow fragment-reassembly buffer: grows by exact-size `new (std::nothrow)`
 *        reallocation, so heap exhaustion drops the in-flight message instead of
 *        aborting the node.
 *
 * `std::vector` is unusable here: under `-fno-exceptions` its throwing allocator
 * turns a failed growth into `abort()` via the bad_alloc stub — and the appended
 * chunk is peer-controlled up to kMaxFrameBytes, so reassembly growth MUST be
 * failure-capable (the same backpressure contract as the tx queue). Fragmentation
 * is the rare path (the SPA sends one whole TLV per unfragmented frame) and the
 * total is capped by kMaxFrameBytes, so exact-size regrow-and-copy is the lean
 * choice over capacity doubling.
 */
struct asm_buf_t {
    /** @brief True when no reassembly is in progress. */
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }
    /** @brief Assembled length so far, bytes. */
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    /** @brief The assembled bytes so far (valid until the next append/clear). */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {bytes_.get(), len_}; }
    /** @brief Release the storage (post-deliver / slot-reclaim / drop reset). */
    void clear() noexcept {
        bytes_.reset();
        len_ = 0;
    }
    /**
     * @brief Append @p chunk, nothrow.
     * @retval false Allocation failed — the buffer is cleared (the partial message is
     *               unrecoverable) and the caller drops the message (backpressure).
     */
    [[nodiscard]] bool append(std::span<const std::byte> chunk) noexcept {
        if (chunk.empty()) return true;
        std::unique_ptr<std::byte[]> grown(new (std::nothrow) std::byte[len_ + chunk.size()]);
        if (grown == nullptr) {
            clear();
            return false;
        }
        if (len_ != 0) std::memcpy(grown.get(), bytes_.get(), len_);
        std::memcpy(grown.get() + len_, chunk.data(), chunk.size());
        bytes_ = std::move(grown);
        len_ += chunk.size();
        return true;
    }

   private:
    std::unique_ptr<std::byte[]> bytes_; /**< @brief Owned storage (exact-sized). */
    std::size_t len_ = 0;                /**< @brief Assembled length, bytes. */
};

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
    httpd_ws_link_t* owner = nullptr; /**< @brief Owning link (set once, for reclaim). */
    int fd = -1;                      /**< @brief Peer socket; -1 => free slot. */
    bool open = false;                /**< @brief True between handshake and close. */
    std::string name;                 /**< @brief `<ip>:<port>` of the peer. */
    asm_buf_t asm_buf;                /**< @brief RFC 6455 fragment reassembly (nothrow). */
    std::uint8_t tx_drops = 0;        /**< @brief Consecutive TX-enqueue drops (peers_m_). */
    peer_endpoint_t endpoint;         /**< @brief The directed facade `peer_link` returns. */
};

namespace {

/** @brief One queued outbound frame: the payload is gather-copied ONCE, nothrow, so
 *         it outlives the send() caller's spans until the httpd task drains the work
 *         item. A `unique_ptr` buffer, never a `std::vector` — the vector's THROWING
 *         allocator inside the braced initializer defeated the `new (std::nothrow)`
 *         guard on the shell: under `-fno-exceptions` a reply-sized copy hitting heap
 *         exhaustion aborted the node (the Gorshok browser-session crash). */
struct tx_work_t {
    httpd_handle_t handle;                /**< @brief Owning httpd instance. */
    int fd;                               /**< @brief Destination peer socket. */
    std::unique_ptr<std::byte[]> payload; /**< @brief The gathered frame bytes. */
    std::size_t len;                      /**< @brief Frame length, bytes. */
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
    // Suppress departure notifications for the session closes THIS teardown provokes
    // (httpd_stop closes every session, re-entering on_session_closed) — the routing
    // plane the notifier targets may be tearing down alongside us.
    stopping_.store(true, std::memory_order_relaxed);
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
    if (err != ESP_OK) return err;                    // socket error => httpd closes the session
    if (frame.len > kMaxFrameBytes) return ESP_FAIL;  // abusive frame => drop the peer

    // Pass 2: ALWAYS drain the payload — even a frame type we ignore must be consumed,
    // or its bytes stay in the stream and the next recv reads them as a frame header
    // (TCP-stream misalignment). Only then decide what to do with it. The buffer is
    // nothrow (frame.len is peer-controlled up to kMaxFrameBytes; a throwing
    // std::vector would abort the node on heap exhaustion under -fno-exceptions);
    // on OOM the payload cannot be drained, so fail the handler — httpd closes just
    // this session (backpressure), never the whole node.
    std::unique_ptr<std::byte[]> payload;
    if (frame.len != 0) {
        payload.reset(new (std::nothrow) std::byte[frame.len]);
        if (payload == nullptr) {
            ESP_LOGW(kTag, "rx alloc failed (len=%u) - closing session", (unsigned)frame.len);
            return ESP_FAIL;
        }
        frame.payload = reinterpret_cast<std::uint8_t*>(payload.get());
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    const std::span<const std::byte> body(payload.get(), frame.len);
    // Only data frames carry a TLV (control frames are httpd's — handle_ws_control_frames
    // is off); a stray TEXT/PONG is now drained and ignored.
    if (frame.type != HTTPD_WS_TYPE_BINARY && frame.type != HTTPD_WS_TYPE_CONTINUE) return ESP_OK;

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
            if (s->open && s->fd == fd) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            // Admission cap: refuse cleanly (ESP_FAIL => httpd closes the socket).
            if (max_peers_ != 0) {
                std::size_t open_n = 0;
                for (const auto& s : slots_)
                    if (s->open) ++open_n;
                if (open_n >= max_peers_) {
                    ESP_LOGW(kTag, "peer refused: at max_peers=%u", (unsigned)max_peers_);
                    return ESP_FAIL;
                }
            }
            for (const auto& s : slots_)
                if (s->fd < 0) {
                    slot = s.get();
                    break;
                }  // reuse a departed slot
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
        deliver(peer, body);  // unfragmented: deliver borrowed, no extra copy
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CONTINUE && slot->asm_buf.empty())
        return ESP_OK;  // stray CONTINUE with no assembly open — drop
    if (frame.type == HTTPD_WS_TYPE_BINARY)
        slot->asm_buf.clear();  // a BINARY mid-assembly discards the stale partial
    if (slot->asm_buf.size() + body.size() > kMaxFrameBytes) {
        slot->asm_buf.clear();
        return ESP_OK;  // reassembly would exceed the cap — drop the message
    }
    if (!slot->asm_buf.append(body)) {
        ESP_LOGW(kTag, "reassembly alloc failed - message dropped");
        return ESP_OK;  // nothrow growth failed: drop the message, keep the peer
    }
    if (frame.final) {
        deliver(peer, slot->asm_buf.bytes());
        slot->asm_buf.clear();
    }
    return ESP_OK;
}

void httpd_ws_link_t::on_session_closed(void* ctx) {
    auto* const slot = static_cast<session_t*>(ctx);
    if (slot == nullptr || slot->owner == nullptr) return;
    slot->owner->reclaim_slot(slot);
}

void httpd_ws_link_t::reclaim_slot(session_t* slot) {
    std::string departed;
    bool was_open;
    {
        const std::lock_guard lock(peers_m_);
        was_open = slot->open;
        departed = std::move(slot->name);
        slot->open = false;
        slot->fd = -1;
        slot->name.clear();
        slot->asm_buf.clear();
        slot->tx_drops = 0;
    }
    // Departure seam (RFC-0009 §D extended to peer departure): a browser tab that
    // hung up leaves its subscriber edges behind — fire the routing plane's eviction
    // hook (fwd_router_t::link_down via the installed notifier) LAST, with peers_m_
    // released (the notifier re-enters router → graph locks). Only a session that
    // completed its handshake (open) can have flowed subscribes. Suppressed while the
    // link itself is being torn down (stopping_): the routing plane may already be
    // gone, and whole-node teardown needs no per-peer eviction. A TX-failure-triggered
    // close (tx_work / note_tx_result) arrives here through the same free_ctx path, so
    // the departed peer's subscriber edges are evicted too; was_open (flipped under
    // peers_m_ on the first pass) keeps the notifier single-fire per session.
    if (was_open && !departed.empty() && !stopping_.load(std::memory_order_relaxed)) {
        if (peer_named_)
            notify_peer_down(departed);
        else
            notify_down();
    }
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

void httpd_ws_link_t::queue_send(int fd, std::span<const std::span<const std::byte>> iov) {
    if (handle_ == nullptr || fd < 0) return;
    // Gather-copy the payload ONCE: httpd_queue_work is asynchronous, so the caller's
    // spans are gone by the time the httpd task runs tx_work. Heap, never a fixed
    // static buffer — and nothrow END TO END: the previous std::vector copy inside
    // the braced initializer allocated with the THROWING allocator, so the
    // `new (std::nothrow)` guard only covered the work-item shell and a reply-sized
    // copy hitting heap exhaustion aborted the node under -fno-exceptions. An
    // allocation failure is now exactly the drop contract below: note_tx_result
    // counts it and the streak closes the session.
    std::size_t total = 0;
    for (const auto& part : iov) total += part.size();
    std::unique_ptr<std::byte[]> buf(new (std::nothrow) std::byte[total]);
    tx_work_t* work = nullptr;
    if (buf != nullptr) {
        std::byte* p = buf.get();
        for (const auto& part : iov) {
            if (!part.empty()) std::memcpy(p, part.data(), part.size());
            p += part.size();
        }
        // If the shell allocation fails the initializer never runs, so `buf` is not
        // moved-from and frees itself on return — no leak either way.
        work = new (std::nothrow) tx_work_t{handle_, fd, std::move(buf), total};
    }
    bool queued = false;
    if (work != nullptr) {  // work == nullptr => OOM: drop this frame (backpressure)
        queued = httpd_queue_work(handle_, &httpd_ws_link_t::tx_work, work) == ESP_OK;
        if (!queued) delete work;  // could not enqueue — no leak
    }
    // Either drop kind is counted; kMaxConsecutiveTxDrops in a row closes the session.
    note_tx_result(fd, queued, total);
}

void httpd_ws_link_t::queue_send(int fd, std::span<const std::byte> frame) {
    // One-span sugar over the gather form — the single copy/backpressure locus.
    const std::span<const std::byte> one[] = {frame};
    queue_send(fd, std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::note_tx_result(int fd, bool queued, std::size_t bytes) {
    bool close_now = false;
    std::string peer;
    {
        const std::lock_guard lock(peers_m_);
        session_t* slot = nullptr;
        for (const auto& s : slots_)
            if (s->open && s->fd == fd) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) return;  // peer departed between the fd snapshot and now
        if (queued) {
            slot->tx_drops = 0;  // a drop streak is CONSECUTIVE — any success resets it
            return;
        }
        if (slot->tx_drops < kMaxConsecutiveTxDrops) ++slot->tx_drops;
        close_now = slot->tx_drops >= kMaxConsecutiveTxDrops;
        peer = slot->name;
    }
    ESP_LOGW(kTag, "tx enqueue drop (queue full / OOM) peer=%s fd=%d len=%u%s", peer.c_str(), fd,
             (unsigned)bytes, close_now ? " - closing session" : "");
    // At the streak cap the session is broken, not bursty: close it so the peer's
    // onclose fires and it reconnects, instead of silently missing frames forever.
    // trigger_close rides the same control socket a full queue starves, so it can
    // fail here too — the streak stays at the cap and the next drop re-triggers.
    if (close_now && httpd_sess_trigger_close(handle_, fd) != ESP_OK)
        ESP_LOGW(kTag, "trigger_close failed fd=%d (will retry on next drop)", fd);
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
        f.payload = reinterpret_cast<std::uint8_t*>(work->payload.get());
        f.len = work->len;
        const esp_err_t err = httpd_ws_send_frame_async(work->handle, work->fd, &f);
        if (err != ESP_OK) {
            // DROP the frame; do NOT close the session (#481). A single failed async
            // send means the peer missed ONE frame it can retry — NOT that the socket
            // is dead. The load-bearing case: a large reply (e.g. the composed-root
            // snapshot, ~12.7 KB) whose one contiguous WS frame times out SO_SNDTIMEO on
            // a fragmented heap while the SAME socket still delivers small frames fine.
            // Closing here tore the whole session down and killed the peer's follow-on
            // small requests ("transport closed") — the dead-web-ui churn. Dropping
            // keeps the socket alive, so the peer's next (small) request succeeds and its
            // own deadline/retry recovers the missed reply. A genuinely dead TCP is still
            // reaped by lwIP -> free_ctx (slot reclaim + subscriber eviction), and a
            // jammed WORK QUEUE still closes via note_tx_result's enqueue-drop streak;
            // this path only stops a single oversized send from tearing a healthy socket.
            ESP_LOGW(kTag, "ws send failed (%s) fd=%d len=%u - dropped, socket kept",
                     esp_err_to_name(err), work->fd, (unsigned)work->len);
        }
    }
    delete work;
}

void httpd_ws_link_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::send(std::span<const std::span<const std::byte>> iov) {
    // Broadcast: snapshot the open fds under the lock, then enqueue unlocked — the
    // per-session drop accounting inside queue_send takes peers_m_ itself. Overriding
    // the iovec entry point means a rope reply is gathered ONCE per peer, straight
    // into the queued work buffer — the base default's gather-into-a-temporary would
    // double-buffer a large reply (flatten temp + tx copy live simultaneously), the
    // heap spike behind the on-device OOM abort.
    std::vector<int> fds;
    {
        const std::lock_guard lock(peers_m_);
        fds.reserve(slots_.size());
        for (const auto& s : slots_)
            if (s->open) fds.push_back(s->fd);
    }
    for (const int fd : fds) queue_send(fd, iov);
}

void httpd_ws_link_t::peer_endpoint_t::send(std::span<const std::byte> frame) {
    const std::span<const std::byte> one[] = {frame};
    send(std::span<const std::span<const std::byte>>(one));
}

void httpd_ws_link_t::peer_endpoint_t::send(std::span<const std::span<const std::byte>> iov) {
    // The directed reply path (fwd_router hands the reply rope's iovec here): one
    // nothrow gather into the tx work item, no intermediate flatten temporary.
    if (owner_ == nullptr || slot_ == nullptr) return;
    int fd = -1;
    {
        const std::lock_guard lock(owner_->peers_m_);
        if (!slot_->open) return;  // departed => no-op
        fd = slot_->fd;
    }
    owner_->queue_send(fd, iov);
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
