/**
 * @file
 * @brief Frame ingress (ADR-0087 §1): each frame into a segment of its exact length, drawn from
 *        the receiving link's own source; small frames from one read, large bodies received
 *        straight into their segment.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One module for every stream link. A link supplies only its framing, as two calls on a
 * `Framing` object it passes in:
 *
 * - `ingress_header_t header(std::span<const std::byte> have)`: judge the header bytes read so
 *   far (a body it announces is under 4 GiB: the link's receive cap holds it there). `MORE` names
 * how many header bytes it needs in all; `CLOSE` refuses the stream (an announced length above
 * `max_frame`, a protocol breach) BEFORE anything is drawn; `BODY` says how many bytes the header
 * took and how long the body is.
 * - `bool body(view::rx_block_t& blk, std::size_t len)`: one whole body. `blk` holds it at
 *   its exact length, or is empty when the source refused the draw (`len != 0`), which the
 *   link sheds and counts. The link takes the bytes out of `blk` (`blk.take(len)`) to hand
 *   them up. False closes the link.
 *
 * What the module owns is the state between reads: the header bytes read so far (at most
 * `MaxHeader`, the framing's largest header) and the body's segment while it fills. Nothing
 * else is kept. Between frames a read offers the header slot and then a stack scratch of
 * `ReadAhead` bytes: every frame whole in what arrived is copied into its exact segment, a
 * partial header stays in the slot, and a partial body has its exact segment drawn and its
 * bytes copied in, all before the call returns, so the scratch holds nothing between reads
 * and a lone small frame costs one read. A body in progress is read straight into its
 * segment, with the header slot taking what follows in the same `recvmsg`, so frames that
 * arrive back to back cost one read each; past that header, at most `MaxHeader - 2` bytes of
 * the next body are copied.
 *
 * Native-window hold (ADR-0081 §2) is kept: ingress says "not yet" by not reading, so unread
 * bytes wait in the kernel's window.
 */

#pragma once

#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "libtracer/config.hpp"
#include "libtracer/mem_heap.hpp"

namespace tr::net {

/** @brief What a link's framing made of the header bytes @ref frame_ingress_t has read. */
struct ingress_header_t {
    /** @brief The verdict. */
    enum class kind_t : std::uint8_t {
        MORE,  /**< @brief Not whole yet: the header needs @ref size bytes in all. */
        BODY,  /**< @brief Whole: it took @ref size bytes and a body of @ref body bytes follows. */
        CLOSE, /**< @brief The header broke the link's rules: close, drawing nothing. */
    };
    kind_t kind = kind_t::MORE; /**< @brief The verdict. */
    std::size_t size = 0;       /**< @brief Header bytes (needed on `MORE`, taken on `BODY`). */
    std::size_t body = 0;       /**< @brief The body's length, on `BODY`. */
};

/**
 * @brief The frame-ingress state of one stream (see the file comment): the header being read,
 *        then the exact-length segment its body is received into.
 *
 * Used by the stream's one receive context only.
 *
 * @tparam MaxHeader The framing's largest header, in bytes.
 * @tparam ReadAhead The stack scratch a read between frames offers after the header slot, in
 *                   bytes (it also takes a refused body's bytes, which are shed).
 */
template <std::size_t MaxHeader, std::size_t ReadAhead>
class frame_ingress_t {
   public:
    /**
     * @brief Read what @p fd has, straight into where each byte belongs, and hand up every
     *        frame it completes.
     *
     * Reads again only while a read filled everything it offered and completed no frame, so
     * a lone frame up to about `ReadAhead` bytes costs one read, a larger one two, and one
     * busy peer cannot hold the caller past its next frame.
     *
     * @param fd      The stream's socket.
     * @param rx      The receiving link's own source, which every body is drawn from.
     * @param framing The link's framing (see the file comment).
     * @return False when the stream must close: the peer closed it, a read failed, or the
     *         framing said so.
     */
    template <class Framing>
    [[nodiscard]] bool read(int fd, mem::mem_backend_t& rx, Framing& framing) {
        for (bool more = true; more;) {
            // A body in progress takes its remaining bytes (or, refused by the source, this
            // scratch as throwaway space) and the header slot whatever follows. Between
            // frames the header slot comes first and the scratch takes up to `ReadAhead`
            // bytes more, which this call moves into their frames before it returns.
            std::array<std::byte, ReadAhead> scratch;
            const bool between = !in_body_;
            const std::size_t room = body_len_ - body_have_;
            const ::iovec hdr{hdr_.data() + hdr_have_, MaxHeader - hdr_have_};
            const ::iovec ahead{scratch.data(), scratch.size()};
            const ::iovec into = blk_.seg ? ::iovec{blk_.frame(body_len_).data() + body_have_, room}
                                          : ::iovec{scratch.data(), std::min(room, scratch.size())};
            std::array<::iovec, 2> iov = between ? std::array{hdr, ahead} : std::array{into, hdr};
            ::msghdr msg{};
            msg.msg_iov = iov.data();
            msg.msg_iovlen = iov.size();
            const ::ssize_t n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
            if (n <= 0) return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);

            const auto got = static_cast<std::size_t>(n);
            const std::size_t first = std::min(got, iov[0].iov_len);
            const std::size_t second = got - first;
            body_have_ += static_cast<std::uint32_t>(between ? 0 : first);
            hdr_have_ += static_cast<std::uint8_t>(between ? first : second);
            const int done = advance(rx, framing);
            const int fed =
                !between || done < 0
                    ? 0
                    : feed(std::span<const std::byte>(scratch.data(), second), rx, framing);
            if (done < 0 || fed < 0) return false;
            more = got == iov[0].iov_len + iov[1].iov_len && done + fed == 0;
        }
        return true;
    }

    /**
     * @brief Frame bytes that were already read elsewhere (the bytes a handshake read past
     *        its own end, a wake-up read's scratch), exactly as if they had arrived on the
     *        socket: each frame whole in them is copied into its exact segment, a partial
     *        header kept, a partial body's segment drawn now and its bytes copied in.
     *
     * @return How many bodies it handed up, or -1 when the framing closed the stream.
     */
    template <class Framing>
    [[nodiscard]] int feed(std::span<const std::byte> in, mem::mem_backend_t& rx,
                           Framing& framing) {
        int done = 0;
        while (!in.empty()) {
            std::size_t take = 0;
            if (in_body_) {
                take = std::min<std::size_t>(body_len_ - body_have_, in.size());
                if (blk_.seg)
                    std::memcpy(blk_.frame(body_len_).data() + body_have_, in.data(), take);
                body_have_ += static_cast<std::uint32_t>(take);
            } else {
                take = std::min<std::size_t>(MaxHeader - hdr_have_, in.size());
                std::memcpy(hdr_.data() + hdr_have_, in.data(), take);
                hdr_have_ += static_cast<std::uint8_t>(take);
            }
            in = in.subspan(take);
            const int d = advance(rx, framing);
            if (d < 0) return -1;
            done += d;
        }
        return done;
    }

    /** @brief Forget the stream: drop a half-read header and release a half-filled body. */
    void reset() noexcept {
        blk_ = {};
        hdr_have_ = 0;
        body_len_ = 0;
        body_have_ = 0;
        in_body_ = false;
    }

   private:
    /**
     * @brief Hand up a body that is whole, then judge the header store: start each body it
     *        announces, moving the bytes read past its header into it.
     *
     * @return How many bodies it handed up, or -1 when the framing closed the stream.
     */
    template <class Framing>
    [[nodiscard]] int advance(mem::mem_backend_t& rx, Framing& framing) {
        int done = 0;
        while (true) {
            if (in_body_) {
                if (body_have_ < body_len_) return done;
                in_body_ = false;
                ++done;
                const bool keep = framing.body(blk_, body_len_);
                blk_ = {};
                if (!keep) return -1;
                continue;
            }
            const ingress_header_t h =
                framing.header(std::span<const std::byte>(hdr_.data(), hdr_have_));
            if (h.kind != ingress_header_t::kind_t::BODY)
                return h.kind == ingress_header_t::kind_t::MORE ? done : -1;
            // Drawn at the exact length, from the receiving link's own source; a refusal
            // leaves `blk_` empty and the body is read past and shed. The framing's cap keeps
            // a body under 4 GiB, so its length fits the 32-bit count.
            in_body_ = true;
            body_len_ = static_cast<std::uint32_t>(h.body);
            if (h.body != 0) blk_ = view::alloc_rx(rx, h.body, graph::kShareThresholdBytes);
            const std::size_t rest = hdr_have_ - h.size;
            body_have_ = static_cast<std::uint32_t>(std::min(rest, h.body));
            if (blk_.seg && body_have_ != 0)
                std::memcpy(blk_.frame(body_len_).data(), hdr_.data() + h.size, body_have_);
            hdr_have_ = static_cast<std::uint8_t>(rest - body_have_);
            if (hdr_have_ != 0)
                std::memmove(hdr_.data(), hdr_.data() + h.size + body_have_, hdr_have_);
        }
    }

    static_assert(MaxHeader < 256, "the header count is one byte");

    std::array<std::byte, MaxHeader> hdr_; /**< @brief Header bytes read and not yet judged. */
    std::uint8_t hdr_have_ = 0;            /**< @brief How many of `hdr_` are filled. */
    bool in_body_ = false;                 /**< @brief A body is being received. */
    view::rx_block_t blk_;                 /**< @brief The current body's segment. */
    std::uint32_t body_len_ = 0;           /**< @brief The current body's length. */
    std::uint32_t body_have_ = 0;          /**< @brief Body bytes received so far. */
};

}  // namespace tr::net
