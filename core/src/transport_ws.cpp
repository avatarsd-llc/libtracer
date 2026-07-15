/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_ws.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "libtracer/mem_heap.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/ws.hpp"

namespace tr::net {

namespace {

/**
 * @brief RFC 6455 fragmented-message reassembly as ROPE CHAINING (ADR-0053 §5): each data fragment
 *        becomes one owning link (the copy out of the reused connection buffer is the legitimate
 *        substrate-boundary copy; chaining the fragments is pointer-linking, never a memcpy).
 *
 * One assembler per connection,
 * used only on its recv thread.
 */
struct ws_assembler_t {
    tr::view::rope_t partial;
    bool assembling = false;

    void reset() {
        partial = tr::view::rope_t{};
        assembling = false;
    }

    /**
     * @brief Feed one data frame (BINARY or CONT).
     *
     * Returns the completed message as a
     * rope when this frame finishes one, std::nullopt otherwise. A BINARY that
     * arrives mid-assembly is an RFC 6455 protocol error: the stale assembly is
     * dropped and the new message starts. A stray CONT (no assembly open) is
     * dropped. An allocation failure drops the whole message (backpressure).
     */
    std::optional<tr::view::rope_t> on_data(ws::opcode_t op, bool fin,
                                            std::span<const std::byte> payload) {
        if (op == ws::opcode_t::BINARY && assembling) reset();             // protocol error
        if (op == ws::opcode_t::CONT && !assembling) return std::nullopt;  // stray

        const std::optional<tr::view::view_t> link = tr::view::over_bytes(payload);
        if (!link) {  // allocation failure => drop the message (backpressure)
            reset();
            return std::nullopt;
        }
        partial.append(*link);
        assembling = true;
        if (!fin) return std::nullopt;
        tr::view::rope_t done = std::move(partial);
        reset();
        return done;
    }
};

}  // namespace

namespace {

/**
 * @brief Case-insensitive search for an HTTP header and return its trimmed value.
 *
 * `request` is the raw header block; `name` is lowercase (e.g. "sec-websocket-key").
 */
std::string header_value(std::string_view request, std::string_view name) {
    std::string lower(request);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = lower.find(name, pos)) != std::string::npos) {
        // Must sit at the start of a header line (start of buffer or after a newline).
        if (pos != 0 && request[pos - 1] != '\n') {
            pos += name.size();
            continue;
        }
        std::size_t colon = request.find(':', pos + name.size());
        if (colon == std::string_view::npos) return {};
        std::size_t eol = request.find("\r\n", colon);
        if (eol == std::string_view::npos) eol = request.size();
        std::string_view val = request.substr(colon + 1, eol - colon - 1);
        std::size_t b = val.find_first_not_of(" \t");
        if (b == std::string_view::npos) return {};
        std::size_t e = val.find_last_not_of(" \t");
        return std::string(val.substr(b, e - b + 1));
    }
    return {};
}

}  // namespace

/**
 * @brief One peer slot: the connection state of a single inbound WebSocket client.
 *
 * Slots are recycled in place across connections (the header's bounded-steady-state
 * contract) and never freed before the server, so the @ref endpoint facade
 * `peer_link` hands out stays pointer-valid for the server's lifetime. Threading:
 * @ref fd / @ref open are atomics (senders read them under `write_m_`); @ref name
 * is guarded by `peers_m_` (cross-thread reads from `peer_link`/`enumerate_peers`);
 * the buffers and the assembler are recv-thread-only.
 */
struct transport_ws_server::session_t {
    std::atomic<int> fd{-1};       /**< @brief The peer socket; -1 ⇒ free slot. */
    std::atomic<bool> open{false}; /**< @brief True once the 101 handshake completed. */
    std::string name;              /**< @brief The peer's name, `<ip>:<port>`. */
    std::string hs_buf;            /**< @brief HTTP Upgrade request accumulation. */
    std::vector<std::byte> buf;    /**< @brief Stream bytes → frame reassembly. */
    ws_assembler_t assembler;      /**< @brief RFC 6455 fragment reassembly. */
    peer_endpoint_t endpoint;      /**< @brief The directed facade `peer_link` returns. */
};

transport_ws_server::transport_ws_server(std::uint16_t bind_port, std::size_t max_peers,
                                         bool peer_named)
    : max_peers_(max_peers), peer_named_(peer_named) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    // SOMAXCONN: the OS's own accept-queue bound — admission is per-connection in
    // accept_peer (the max_peers deployment cap), never a synthetic backlog of 1.
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, SOMAXCONN) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    start([this] { run(); });
}

transport_ws_server::~transport_ws_server() {
    stop_and_join();  // FIRST: the run() thread touches the fds closed below
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (const std::unique_ptr<session_t>& s : slots_) {  // thread joined — nothing races
        const int fd = s->fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) ::close(fd);
    }
}

void transport_ws_server::send(std::span<const std::byte> frame) {
    // Encode ONCE, then one serialized record per open peer. Lock order per the
    // header contract: peers_m_ (slot list stable) → write_m_ (the stream
    // write-serialization invariant, now covering every peer fd).
    const std::vector<std::byte> encoded = ws::encode_frame(ws::opcode_t::BINARY, frame);
    const std::lock_guard plock(peers_m_);
    const std::lock_guard wlock(write_m_);
    for (const std::unique_ptr<session_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed)) continue;
        write_all(s->fd.load(std::memory_order_relaxed), encoded);
    }
}

void transport_ws_server::peer_endpoint_t::send(std::span<const std::byte> frame) {
    if (owner_ == nullptr || slot_ == nullptr) return;
    const std::vector<std::byte> encoded = ws::encode_frame(ws::opcode_t::BINARY, frame);
    const std::lock_guard lock(owner_->write_m_);
    if (!slot_->open.load(std::memory_order_relaxed)) return;  // departed ⇒ no-op
    write_all(slot_->fd.load(std::memory_order_relaxed), encoded);
}

void transport_ws_server::enumerate_peers(const peer_visitor_t& visit) const {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && !s->name.empty()) visit(s->name);
}

transport_t* transport_ws_server::peer_link(std::string_view peer) {
    const std::lock_guard lock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_)
        if (s->open.load(std::memory_order_relaxed) && s->name == peer) return &s->endpoint;
    return nullptr;
}

bool transport_ws_server::close_peer(std::string_view peer) {
    // Find the named open peer under the same lock order senders use
    // (peers_m_ → write_m_), then ::shutdown its socket. We deliberately do NOT
    // call teardown_slot here: it clears the recv-thread-only buffers/assembler,
    // so it must run only on the recv thread. The shutdown wakes that thread's
    // next poll, ::recv returns 0, and the IDENTICAL remote-FIN path recycles the
    // slot — no duplicate teardown logic, no off-thread buffer touch.
    const std::lock_guard plock(peers_m_);
    for (const std::unique_ptr<session_t>& s : slots_) {
        if (!s->open.load(std::memory_order_relaxed) || s->name != peer) continue;
        const std::lock_guard wlock(write_m_);
        const int fd = s->fd.load(std::memory_order_relaxed);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        return true;
    }
    return false;
}

void transport_ws_server::accept_peer() {
    sockaddr_in remote{};
    socklen_t rlen = sizeof(remote);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&remote), &rlen);
    if (fd < 0) return;

    session_t* slot = nullptr;
    {
        const std::lock_guard lock(peers_m_);
        for (const std::unique_ptr<session_t>& s : slots_)
            if (s->fd.load(std::memory_order_relaxed) < 0) {
                slot = s.get();
                break;
            }
        if (slot == nullptr) {
            if (max_peers_ != 0 && slots_.size() >= max_peers_) {
                ::close(fd);  // clean refusal at the deployment cap, not a hung SYN
                return;
            }
            slots_.push_back(std::make_unique<session_t>());
            slot = slots_.back().get();
            slot->endpoint.owner_ = this;
            slot->endpoint.slot_ = slot;
        }
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        slot->name = std::string(ip) + ':' + std::to_string(ntohs(remote.sin_port));
    }
    slot->hs_buf.clear();
    slot->buf.clear();
    slot->assembler.reset();
    slot->open.store(false, std::memory_order_relaxed);
    slot->fd.store(fd, std::memory_order_release);  // publish LAST — the slot is now live
}

void transport_ws_server::service_peer(session_t& s) {
    const int fd = s.fd.load(std::memory_order_relaxed);
    if (fd < 0) return;
    std::array<std::byte, 4096> chunk;
    const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (n <= 0) {  // peer closed the TCP connection, or error
        teardown_slot(s);
        return;
    }

    if (!s.open.load(std::memory_order_relaxed)) {
        // Opening handshake: accumulate the HTTP Upgrade request until CRLFCRLF.
        s.hs_buf.append(reinterpret_cast<const char*>(chunk.data()), static_cast<std::size_t>(n));
        if (s.hs_buf.size() > 16384) {  // runaway request guard
            teardown_slot(s);
            return;
        }
        const std::size_t hdr_end = s.hs_buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) return;  // keep accumulating

        const std::string key =
            header_value(std::string_view(s.hs_buf.data(), hdr_end + 4), "sec-websocket-key");
        if (key.empty()) {
            teardown_slot(s);
            return;
        }
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
        resp += ws::accept_key(key);
        resp += "\r\n\r\n";
        std::vector<std::byte> bytes(resp.size());
        for (std::size_t i = 0; i < resp.size(); ++i) bytes[i] = static_cast<std::byte>(resp[i]);
        {
            const std::lock_guard lock(write_m_);
            write_all(fd, bytes);
        }
        // Bytes pipelined past the header are the start of the frame stream —
        // the old one-peer server dropped them; carry them over.
        const auto* rest = reinterpret_cast<const std::byte*>(s.hs_buf.data()) + hdr_end + 4;
        s.buf.assign(rest, rest + (s.hs_buf.size() - hdr_end - 4));
        s.hs_buf.clear();
        s.hs_buf.shrink_to_fit();
        s.open.store(true, std::memory_order_release);
        if (!drain_frames(s)) teardown_slot(s);
        return;
    }

    s.buf.insert(s.buf.end(), chunk.data(), chunk.data() + n);
    if (!drain_frames(s)) teardown_slot(s);
}

bool transport_ws_server::drain_frames(session_t& s) {
    // Drain every complete frame currently buffered; leftover partial bytes stay
    // for the next read. Returns false when the peer sent CLOSE.
    while (true) {
        auto decoded = ws::decode_frame(s.buf);
        if (!decoded) return true;
        ws::frame_t frame = std::move(decoded->first);
        const std::size_t consumed = decoded->second;
        s.buf.erase(s.buf.begin(), s.buf.begin() + static_cast<std::ptrdiff_t>(consumed));

        switch (frame.op) {
            case ws::opcode_t::BINARY:
            case ws::opcode_t::CONT: {
                // Peer-named slot first (the ADR-0044 bus precedence — the router's
                // wiring), flat transport_t slot as the point-to-point fallback.
                // Tier select per frame (receiver_slot.hpp), so a sink installed
                // mid-stream takes effect on the next data frame. Unfragmented
                // fast path on the span tier: borrowed payload, no owning copy.
                const bool peer_named = peer_rx_.has_any();
                const bool want_rope = peer_named ? peer_rx_.has_rope() : rx_.has_rope();
                if (frame.op == ws::opcode_t::BINARY && frame.fin && !s.assembler.assembling &&
                    !want_rope) {
                    const std::span<const std::byte> payload(frame.payload);
                    if (peer_named)
                        peer_rx_.deliver_borrowed(s.name, payload);
                    else
                        rx_.deliver_borrowed(payload);
                    break;
                }
                auto msg = s.assembler.on_data(frame.op, frame.fin, frame.payload);
                if (!msg) break;  // mid-message (or dropped)
                // The reassembled message IS a rope — one owning link per
                // fragment (ADR-0053 §5): the rope sink takes it as-is; only
                // a span-only sink pays the one materialize (in the slot).
                if (peer_named)
                    peer_rx_.deliver_rope(s.name, std::move(*msg));
                else
                    rx_.deliver_rope(std::move(*msg));
                break;
            }
            case ws::opcode_t::PING: {
                const std::vector<std::byte> pong =
                    ws::encode_frame(ws::opcode_t::PONG, frame.payload);
                const std::lock_guard lock(write_m_);
                write_all(s.fd.load(std::memory_order_relaxed), pong);
                break;
            }
            case ws::opcode_t::CLOSE:
                return false;  // tear this one connection down
            default:
                break;  // TEXT / PONG: ignored
        }
    }
}

void transport_ws_server::teardown_slot(session_t& s) {
    {
        // Stop peer_link/enumerate resolution FIRST, so no new sender targets
        // the dying slot by name.
        const std::lock_guard lock(peers_m_);
        s.name.clear();
    }
    int fd;
    {
        // The stream teardown-under-write-lock invariant, per slot: reset the fd
        // and the open flag under write_m_ BEFORE ::close, so an in-flight send
        // either finished against the still-open fd or observes the reset.
        const std::lock_guard lock(write_m_);
        s.open.store(false, std::memory_order_relaxed);
        fd = s.fd.exchange(-1, std::memory_order_acq_rel);
    }
    if (fd >= 0) ::close(fd);
    s.buf.clear();
    s.buf.shrink_to_fit();
    s.hs_buf.clear();
    s.assembler.reset();
}

void transport_ws_server::run() {
    // ONE poll pass multiplexes the listen socket and every live peer — no
    // per-peer thread (the MCU-shaped choice, #362), bounded to 100 ms so the
    // loop stays shutdown-responsive (the posix_endpoint_t idiom).
    std::vector<pollfd> pfds;
    std::vector<session_t*> pslots;
    while (!stop_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pslots.clear();
        pfds.push_back(pollfd{listen_fd_, POLLIN, 0});
        {
            const std::lock_guard lock(peers_m_);
            for (const std::unique_ptr<session_t>& s : slots_) {
                const int fd = s->fd.load(std::memory_order_relaxed);
                if (fd >= 0) {
                    pfds.push_back(pollfd{fd, POLLIN, 0});
                    pslots.push_back(s.get());
                }
            }
        }
        const int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (pr <= 0) continue;  // timeout or transient error → re-check stop_
        if (stop_.load(std::memory_order_relaxed)) break;
        // Peers first (their events are bound to this pass's fd list), then the
        // accept (which may add a slot).
        for (std::size_t i = 1; i < pfds.size(); ++i)
            if ((pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) service_peer(*pslots[i - 1]);
        if ((pfds[0].revents & POLLIN) != 0) accept_peer();
    }
}

// ---------------------------------------------------------------------------
// transport_ws_client — the dial-out half.
// ---------------------------------------------------------------------------

transport_ws_client::transport_ws_client(const std::string& host, std::uint16_t port) {
    // Seed the per-frame masking-key stream with something that varies between
    // connections (steady_clock + this address). Not crypto-strong — RFC 6455
    // masking exists for proxy/cache safety, not to defend against a peer.
    std::uint64_t seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= reinterpret_cast<std::uintptr_t>(this);
    mask_state_.store(seed, std::memory_order_relaxed);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }

    if (!handshake(fd, host, port)) {
        ::close(fd);
        return;
    }

    conn_fd_.store(fd, std::memory_order_relaxed);
    connected_ = true;
    start([this, fd] { serve(fd); });
}

transport_ws_client::~transport_ws_client() {
    stop_and_join();  // FIRST: serve() touches conn_fd_
    // A leftover fd (never-spawned thread — handshake failed — leaves conn_fd_
    // at -1 anyway) is closed by ~stream_endpoint_t after this body.
}

std::uint32_t transport_ws_client::next_mask_key() {
    // SplitMix64 step → a varied (non-crypto) 32-bit masking key per frame.
    std::uint64_t z = mask_state_.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed) +
                      0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<std::uint32_t>(z);
}

void transport_ws_client::send(std::span<const std::byte> frame) {
    // One serialized MASKED record under write_m_ (the stream_endpoint_t
    // write-serialization invariant).
    send_all_locked(ws::encode_client_frame(ws::opcode_t::BINARY, frame, next_mask_key()));
}

bool transport_ws_client::handshake(int fd, const std::string& host, std::uint16_t port) {
    // A fresh 16-byte nonce (RFC 6455 §4.1) base64'd into Sec-WebSocket-Key.
    std::array<std::byte, 16> nonce{};
    std::uint64_t s = mask_state_.load(std::memory_order_relaxed) ^ 0xD1B54A32D192ED03ull;
    for (auto& b : nonce) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::byte>((s >> 56) & 0xFFu);
    }
    const std::string key = ws::base64(nonce);

    std::string req = "GET / HTTP/1.1\r\nHost: ";
    req += host;
    req += ':';
    req += std::to_string(port);
    req +=
        "\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    req += key;
    req += "\r\nSec-WebSocket-Version: 13\r\n\r\n";

    std::vector<std::byte> bytes(req.size());
    for (std::size_t i = 0; i < req.size(); ++i) bytes[i] = static_cast<std::byte>(req[i]);
    {
        const std::lock_guard lock(write_m_);
        write_all(fd, bytes);
    }

    // Read the response header block (CRLFCRLF), bounded so a stuck peer cannot
    // hang construction forever.
    std::string resp;
    std::array<char, 1024> chunk;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        const int pr = poll_readable(fd);  // one bounded 100 ms readability wait
        if (pr < 0) return false;
        if (pr == 0) continue;  // timeout → re-check stop_
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;  // peer closed / error
        resp.append(chunk.data(), static_cast<std::size_t>(n));
        if (resp.size() > 16384) return false;  // runaway response guard
    }

    if (resp.find("101") == std::string::npos) return false;
    const std::string accept = header_value(resp, "sec-websocket-accept");
    return !accept.empty() && accept == ws::accept_key(key);
}

void transport_ws_client::serve(int fd) {
    ws_assembler_t asm_state;  // per-connection fragment assembly (recv thread only)
    std::vector<std::byte> buf;
    std::array<std::byte, 4096> chunk;

    while (!stop_.load(std::memory_order_relaxed)) {
        const int pr = poll_readable(fd);  // one bounded 100 ms readability wait
        if (pr < 0) break;
        if (pr == 0) continue;  // timeout → re-check stop_

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) break;  // peer closed / error
        buf.insert(buf.end(), chunk.data(), chunk.data() + n);

        // Drain every complete frame; leftover partial bytes stay for next read.
        while (true) {
            auto decoded = ws::decode_frame(buf);
            if (!decoded) break;
            ws::frame_t frame = std::move(decoded->first);
            const std::size_t consumed = decoded->second;
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));

            switch (frame.op) {
                case ws::opcode_t::BINARY:
                case ws::opcode_t::CONT: {
                    // Sink snapshot/tier select lives in the slot (receiver_slot.hpp);
                    // rx_.has_rope() is the per-frame tier query, so a sink installed
                    // mid-stream takes effect on the next data frame.
                    // Unfragmented fast path on the span tier: the borrowed
                    // payload is delivered directly, no owning copy (as before).
                    if (frame.op == ws::opcode_t::BINARY && frame.fin && !asm_state.assembling &&
                        !rx_.has_rope()) {
                        rx_.deliver_borrowed(std::span<const std::byte>(frame.payload));
                        break;
                    }
                    auto msg = asm_state.on_data(frame.op, frame.fin, frame.payload);
                    if (!msg) break;  // mid-message (or dropped)
                    // The reassembled message IS a rope — one owning link per
                    // fragment (ADR-0053 §5): the rope sink takes it as-is; only
                    // a span-only sink pays the one materialize (in the slot).
                    rx_.deliver_rope(std::move(*msg));
                    break;
                }
                case ws::opcode_t::PING: {
                    // A client PONG must be masked, just like client data frames.
                    const std::vector<std::byte> pong =
                        ws::encode_client_frame(ws::opcode_t::PONG, frame.payload, next_mask_key());
                    const std::lock_guard lock(write_m_);
                    write_all(fd, pong);
                    break;
                }
                case ws::opcode_t::CLOSE:
                    goto teardown;  // peer asked to close
                default:
                    break;  // TEXT / PONG: ignored
            }
        }
    }

teardown:
    teardown_peer(fd);  // reset-under-write_m_ then close (stream_endpoint_t)
}

}  // namespace tr::net
