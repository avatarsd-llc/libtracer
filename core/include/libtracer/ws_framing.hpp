/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "libtracer/frame_ingress.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/ws.hpp"
#include "libtracer/ws_assembler.hpp"

/**
 * @file
 * @brief @ref tr::net::ws_framing_t — the one set of RFC 6455 receive rules, shared by every
 *        WebSocket link that frames its own byte stream (#2484).
 */

namespace tr::net {

/** @brief What the body of the frame being received needs from its header (the length is
 *         frame ingress's own). */
struct ws_rx_frame_t {
    ws::opcode_t op = ws::opcode_t::CONT;   /**< @brief Its opcode. */
    bool fin = false;                       /**< @brief Its FIN bit. */
    bool masked = false;                    /**< @brief Its MASK bit. */
    std::array<std::uint8_t, 4> mask_key{}; /**< @brief Its masking key, when masked. */
};

/**
 * @brief The RFC 6455 receive rules every end shares, as frame ingress calls them
 *        (`frame_ingress.hpp`): the header rules on the way in, then unmasking, fragment
 *        chaining and the opcode's answer on each whole body.
 *
 * Core's WS server and dial-out client and the ESP-IDF client link frame their streams with
 * it. What differs between them is reached through @p End, the end's own adapter: `cap()`
 * (the live receive cap), `malformed()` and `dropped()` (its counters), `pong(bytes)` (its
 * PONG, masked or not), `closing(bytes)` (its answer to a CLOSE, which may be none) and
 * `hand_up(message)` (its delivery), plus the `frame` (@ref ws_rx_frame_t) and `assembler`
 * (@ref ws_assembler_t) state of the stream being read.
 *
 * @tparam End The receiving end's adapter (see above).
 */
template <class End>
struct ws_framing_t {
    End& end; /**< @brief The receiving end. */

    /** @brief Judge the header bytes read so far; an over-cap length or a protocol breach
     *         closes the stream before anything is drawn. */
    [[nodiscard]] ingress_header_t header(std::span<const std::byte> have) const {
        const ws::header_t h = ws::decode_header(have, end.cap());
        if (h.status == ws::decode_status_t::NEED_MORE)
            return {ingress_header_t::kind_t::MORE, h.size, 0};
        if (h.status == ws::decode_status_t::PROTOCOL_ERROR) {
            // An over-cap length, a §5.5 control breach or a §5.2 reserved opcode: a stream
            // we refuse to keep reading. Count it; the stream is torn down (§7.1.7).
            end.malformed();
            return {ingress_header_t::kind_t::CLOSE, 0, 0};
        }
        end.frame = {h.op, h.fin, h.masked, h.mask_key};
        return {ingress_header_t::kind_t::BODY, h.size, h.len};
    }

    /** @brief One whole body, in its exact segment (empty when the source refused it). */
    [[nodiscard]] bool body(tr::view::rx_block_t& blk, std::size_t len) const {
        if (len != 0 && !blk.seg) {
            // The receiving link's source refused the body (backpressure): the message is
            // shed and counted, the connection lives and re-syncs at the next BINARY. A
            // PING shed here goes unanswered.
            end.dropped();
            if (!ws::is_control_opcode(end.frame.op)) end.assembler.reset();
            return end.frame.op != ws::opcode_t::CLOSE;
        }
        const std::span<std::byte> bytes = blk.seg ? blk.frame(len) : std::span<std::byte>{};
        if (end.frame.masked) ws::detail::xor_mask(bytes, end.frame.mask_key);
        switch (end.frame.op) {
            case ws::opcode_t::BINARY:
            case ws::opcode_t::CONT:
                return data(len != 0 ? blk.take(len) : tr::view::view_t{});
            case ws::opcode_t::PING:
                end.pong(bytes);  // the header rules held it to <= 125 bytes (§5.5)
                return true;
            case ws::opcode_t::CLOSE:
                end.closing(bytes);  // <= 125 bytes as well
                return false;        // tear this one connection down
            default:
                return true;  // TEXT / PONG: ignored (a RESERVED opcode never gets here: #1060)
        }
    }

    /**
     * @brief One data frame's bytes: an unfragmented message goes up as the one view over its
     *        segment, a fragment is chained by the assembler until its message completes.
     */
    [[nodiscard]] bool data(tr::view::view_t piece) const {
        if (end.frame.op == ws::opcode_t::BINARY && end.frame.fin) {
            end.assembler.reset();  // a BINARY mid-assembly drops the stale message (§5.4)
            end.hand_up(tr::view::rope_t{std::move(piece)});
            return true;
        }
        ws_assembler_t::result_t r =
            end.assembler.on_piece(end.frame.op, end.frame.fin, std::move(piece), end.cap());
        // Fragments summing past the cap get the answer a single over-cap frame gets, so the
        // fragmented route is not a way around the bound.
        if (r.status == ws_assembler_t::status_t::OVERSIZE) {
            end.malformed();
            return false;
        }
        // A refused chain is a refused body by another name: shed, counted, and the
        // connection re-syncs at the next BINARY.
        if (r.status == ws_assembler_t::status_t::REFUSED) end.dropped();
        if (r.status == ws_assembler_t::status_t::COMPLETE) end.hand_up(std::move(r.message));
        return true;
    }
};

}  // namespace tr::net
