/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_tcp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"

namespace tr::net {

namespace {

// SIGPIPE would otherwise kill the process if the peer vanishes mid-write.
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

constexpr std::size_t kPrefixBytes = 4;  // the u32-LE length prefix (transport framing)

// A receive timeout lets every blocking read poll stop_ for a clean shutdown
// (the transport_udp SO_RCVTIMEO idiom, applied per connection).
void set_rcv_timeout(int fd) {
    timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Frames are small and latency-sensitive (a READ round-trip is two tiny records);
// Nagle coalescing would serialize them behind ACKs.
void set_nodelay(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Write the gathered iovec entries completely, resuming partial writes (a stream
// write may stop anywhere). sendmsg for MSG_NOSIGNAL; `vec` is consumed (advanced).
void writev_all(int fd, ::iovec* vec, std::size_t count) {
    if (fd < 0) return;
    while (count > 0) {
        msghdr msg{};
        msg.msg_iov = vec;
        msg.msg_iovlen = count;
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // peer gone / error → drop the rest
        }
        std::size_t done = static_cast<std::size_t>(n);
        while (count > 0 && done >= vec->iov_len) {
            done -= vec->iov_len;
            ++vec;
            --count;
        }
        if (count > 0 && done > 0) {
            vec->iov_base = static_cast<std::byte*>(vec->iov_base) + done;
            vec->iov_len -= done;
        }
    }
}

}  // namespace

tcp_transport_t::tcp_transport_t(const std::string& peer_host, std::uint16_t peer_port,
                                 mem::mem_backend_t* backend)
    : backend_(backend) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(peer_port);
    if (::inet_pton(AF_INET, peer_host.c_str(), &peer.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
        ::close(fd);
        return;
    }
    set_rcv_timeout(fd);
    set_nodelay(fd);
    conn_fd_.store(fd, std::memory_order_relaxed);
    thread_ = std::thread([this, fd] {
        serve(fd);
        // Tear down under write_m_ so a concurrent send() never writes to (or
        // reads) a closed/reused fd.
        {
            const std::lock_guard lock(write_m_);
            conn_fd_.store(-1, std::memory_order_relaxed);
        }
        ::close(fd);
    });
}

tcp_transport_t::tcp_transport_t(std::uint16_t bind_port, mem::mem_backend_t* backend)
    : listen_(true), backend_(backend) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
        ::listen(listen_fd_, 1) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        bound_port_ = ntohs(bound.sin_port);

    thread_ = std::thread([this] { run_listen(); });
}

tcp_transport_t::~tcp_transport_t() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    // The serve/accept thread closes the peer fd on exit; this only catches a
    // never-spawned thread (dial failed), so it never double-closes.
    const int leftover = conn_fd_.exchange(-1, std::memory_order_relaxed);
    if (leftover >= 0) ::close(leftover);
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void tcp_transport_t::set_receiver(receiver_t receiver) {
    const std::lock_guard lock(m_);
    receiver_ = std::move(receiver);
}

void tcp_transport_t::set_view_receiver(view_receiver_t receiver) {
    const std::lock_guard lock(m_);
    view_receiver_ = std::move(receiver);
}

void tcp_transport_t::send(std::span<const std::byte> frame) {
    if (frame.size() > kMaxFrame) return;  // the peer would reject it as malformed
    std::array<std::byte, kPrefixBytes> prefix;
    detail::store_le(prefix, static_cast<std::uint32_t>(frame.size()));
    std::array<::iovec, 2> vec{
        ::iovec{prefix.data(), prefix.size()},
        ::iovec{const_cast<std::byte*>(frame.data()), frame.size()},
    };
    // Hold write_m_ across the whole write so (a) the recv thread cannot close and
    // reset conn_fd_ underneath us, and (b) two senders can never interleave their
    // length-prefixed records on the stream; read the fd inside the lock to pair
    // with the teardown.
    const std::lock_guard lock(write_m_);
    writev_all(conn_fd_.load(std::memory_order_relaxed), vec.data(),
               frame.empty() ? 1 : vec.size());
}

void tcp_transport_t::send(std::span<const std::span<const std::byte>> iov) {
    std::size_t total = 0;
    for (const auto& s : iov) total += s.size();
    if (total > kMaxFrame) return;  // the peer would reject it as malformed

    // The prefix rides as the FIRST iovec entry, the rope's spans follow — one
    // gathered record, no flatten copy. The iovec count is small and bounded (a
    // FWD forward/reply is ≤ ~6 spans), so the common case uses a fixed stack
    // array; only an unusually large gather falls back to the heap vector.
    std::array<std::byte, kPrefixBytes> prefix;
    detail::store_le(prefix, static_cast<std::uint32_t>(total));
    constexpr std::size_t kMaxInlineIov = 16;
    std::array<::iovec, kMaxInlineIov + 1> inline_vec;
    std::vector<::iovec> heap_vec;
    ::iovec* vec = inline_vec.data();
    if (iov.size() + 1 > inline_vec.size()) {
        heap_vec.resize(iov.size() + 1);
        vec = heap_vec.data();
    }
    vec[0] = ::iovec{prefix.data(), prefix.size()};
    std::size_t n = 1;
    for (const auto& s : iov) {
        if (s.empty()) continue;  // writev rejects nothing, but skip no-op entries
        vec[n++] = ::iovec{const_cast<std::byte*>(s.data()), s.size()};
    }
    const std::lock_guard lock(write_m_);
    writev_all(conn_fd_.load(std::memory_order_relaxed), vec, n);
}

bool tcp_transport_t::read_exact(int fd, std::byte* dst, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        const ssize_t n = ::recv(fd, dst + off, len - off, 0);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) return false;  // peer closed the connection
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;  // receive timeout → re-check stop_ and resume the partial read
        return false;  // hard error
    }
    return true;
}

bool tcp_transport_t::drain(int fd, std::size_t len) {
    // Backpressure discard: the frame must still leave the stream (the next
    // prefix sits right behind it), so it is read into a stack scratch and dropped.
    std::array<std::byte, 4096> scratch;
    while (len > 0) {
        const std::size_t chunk = len < scratch.size() ? len : scratch.size();
        if (!read_exact(fd, scratch.data(), chunk)) return false;
        len -= chunk;
    }
    return true;
}

void tcp_transport_t::serve(int fd) {
    std::array<std::byte, kPrefixBytes> prefix;
    while (!stop_.load(std::memory_order_relaxed)) {
        // Read the 4-byte length prefix, reassembling it across TCP segment
        // boundaries (read_exact resumes partial reads through receive timeouts).
        if (!read_exact(fd, prefix.data(), prefix.size())) return;
        const std::size_t len = detail::load_le<std::uint32_t>(prefix);
        if (len == 0) continue;  // an empty record carries no TLV — a no-op
        if (len > kMaxFrame) {
            // A prefix beyond the cap is malformed (corrupt or hostile): count it
            // and tear the connection down — a desynced stream cannot be re-framed.
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Both receivers are installed before frames flow (the set_receiver
        // contract); snapshot them before the body read.
        view_receiver_t view_receiver;
        receiver_t receiver;
        {
            const std::lock_guard lock(m_);
            view_receiver = view_receiver_;
            receiver = receiver_;
        }

        // ADR-0042 §2/§4: reassemble the frame into ONE refcounted segment drawn
        // from the injected backend, read straight off the socket — no library
        // buffer, no copy. Exhaustion is backpressure: drain the frame off the
        // stream (framing sync survives), drop it, tick the counter — never an OOM.
        view::segment_ptr_t seg = view::segment_ptr_t::adopt(backend_->alloc(len));
        if (!seg) {
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            if (!drain(fd, len)) return;
            continue;
        }
        if (!read_exact(fd, seg->bytes.data(), len)) return;

        if (view_receiver) {
            // Hand the frame up OWNING (narrowed to the frame length) — the
            // receiver may pin, subview, or rope it beyond this call.
            view_receiver(view::view_t::over(std::move(seg)).subview(0, len));
        } else if (receiver) {
            // Borrowed-span delivery from the same segment bytes; the segment is
            // released when the callback returns.
            receiver(std::span<const std::byte>(seg->bytes.data(), len));
        }
    }
}

void tcp_transport_t::run_listen() {
    while (!stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{.fd = listen_fd_, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr <= 0) continue;  // timeout / error → re-check stop_

        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        set_rcv_timeout(fd);
        set_nodelay(fd);
        conn_fd_.store(fd, std::memory_order_relaxed);

        serve(fd);

        // Tear down under write_m_ so a concurrent send() never writes to (or
        // reads) a closed/reused fd; then re-accept the next peer.
        {
            const std::lock_guard lock(write_m_);
            conn_fd_.store(-1, std::memory_order_relaxed);
        }
        ::close(fd);
    }
}

}  // namespace tr::net
