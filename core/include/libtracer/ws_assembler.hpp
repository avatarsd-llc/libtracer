/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "libtracer/mem_source.hpp"
#include "libtracer/placement.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/ws.hpp"

/**
 * @file
 * @brief @ref tr::net::ws_assembler_t — the one RFC 6455 fragmented-message reassembly,
 *        shared by every WebSocket link that receives fragments into segments (#2483).
 */

namespace tr::net {

/**
 * @brief RFC 6455 fragmented-message reassembly as ROPE CHAINING (ADR-0053 §5): each data
 *        fragment is one owning link and chaining them is pointer-linking, never a memcpy.
 *
 * Only a fragmented message needs it: a link hands an unfragmented frame up as the one view
 * over the segment it was received into (ADR-0087 §1). One assembler per connection, used
 * only on that connection's receive thread. Core's WS server and dial-out client feed it
 * from their frame ingress; the ESP-IDF httpd link feeds it from `httpd_ws_recv_frame`.
 *
 * The links are held in a chain drawn from the receiving link's own source, not the process
 * heap, and the receive cap bounds what a message COSTS, not its payload alone: each fragment
 * is charged its bytes plus @ref kFragmentCost, the segment it was received into and its
 * chain slot. Without that charge a message of empty or one-byte fragments held a segment and
 * a link per fragment while its payload total stayed far under the cap.
 *
 * What it answers for each fragment, and what a link does with it:
 *  - a BINARY starts a message; one that arrives mid-assembly is an RFC 6455 protocol error,
 *    so the stale message is dropped and the new one kept;
 *  - a CONT with no message open is stray and dropped, so after any reset the link re-syncs
 *    at the next BINARY;
 *  - `OVERSIZE` (@ref status_t): the charged cost passed the cap; the link closes the connection,
 *    as it does for one over-cap frame (§7.1.7);
 *  - `REFUSED` (@ref status_t): a store refused the chain; the message is shed and counted, the
 *    connection lives (receiver pays).
 *
 * A TEXT frame never reaches it: the links ignore TEXT before reassembly, so a TEXT frame
 * between two fragments leaves the message open. Only BINARY and CONT are passed in.
 */
struct ws_assembler_t {
    /** @brief What chaining one data fragment produced. */
    enum class status_t : std::uint8_t {
        PENDING,  /**< @brief Mid-message (or a stray CONT): nothing to deliver yet. */
        COMPLETE, /**< @brief @ref result_t::message holds the finished message. */
        OVERSIZE, /**< @brief The message's charged cost grew past the receive cap: the link
                              fails the connection (§7.1.7) and counts it. */
        REFUSED,  /**< @brief A store refused the chain: the message is shed and counted, the
                              connection lives (receiver pays). */
    };

    /** @brief One @ref on_piece outcome: a status, and (on `COMPLETE`) the message. */
    struct result_t {
        status_t status = status_t::PENDING; /**< @brief Which outcome. */
        view::rope_t message; /**< @brief The finished message, meaningful only on `COMPLETE`. */
    };

    /** @brief What a fragment costs beyond its payload: its segment's header, the receive
     *         loan room a link may put in front of the bytes, and its chain slot. */
    static constexpr std::size_t kFragmentCost =
        sizeof(view::segment_t) + mem::kRxLoanBytes + sizeof(view::view_t);

    /** @brief An idle assembler whose chain will be drawn from @p rx, the link's own source. */
    explicit ws_assembler_t(mem::block_source_t& rx) noexcept : chain(rx) {}

    mem::block_array_t<view::view_t> chain; /**< @brief The message's fragments. */
    std::size_t charged = 0;                /**< @brief What they cost against the cap. */

    /** @brief True while a message is open: its first fragment is in the chain. */
    [[nodiscard]] bool assembling() const noexcept { return !chain.empty(); }

    /** @brief Drop the message being assembled and hand its chain back to the source. */
    void reset() noexcept {
        chain = mem::block_array_t<view::view_t>(chain.source());
        charged = 0;
    }

    /**
     * @brief Chain one owning data fragment (BINARY or CONT) into the message.
     *
     * Returns the completed message as a rope when this fragment finishes one, `PENDING`
     * otherwise. The assembler is idle again after every outcome but `PENDING`, so a link
     * that delivers the message touches no assembler state afterwards: delivery may run code
     * that tears the link down.
     *
     * @param op    The fragment's opcode: BINARY or CONT.
     * @param fin   Its FIN bit.
     * @param piece The fragment's bytes, owned (an empty view for an empty fragment).
     * @param cap   The effective receive cap the message's CHARGED cost may not exceed. The
     *              per-frame header check bounds one fragment; without this one a peer walks
     *              past the cap a few bytes at a time across a million CONTs (#872).
     */
    result_t on_piece(ws::opcode_t op, bool fin, view::view_t piece, std::size_t cap) {
        if (op == ws::opcode_t::BINARY)
            reset();
        else if (!assembling())
            return {};  // stray
        // Written as a subtraction because `charged + cost` can wrap. `charged <= cap` holds
        // by induction (every earlier fragment passed this same test), so the difference is
        // well-defined; the header check holds `piece.length` to the cap, so `cost` cannot
        // wrap either.
        const std::size_t cost = piece.length + kFragmentCost;
        if (cost > cap - charged) {
            reset();
            return {status_t::OVERSIZE, {}};
        }
        if (!chain.push_back(std::move(piece))) {
            reset();
            return {status_t::REFUSED, {}};
        }
        charged += cost;
        if (!fin) return {};
        // The delivered rope holds the links itself, in a chain from the same source (the
        // receiver pays); reserving them first makes a refusal backpressure instead of an
        // abort.
        result_t done{status_t::REFUSED, view::rope_t(chain.source())};
        if (done.message.try_reserve(chain.size())) {
            for (view::view_t& link : chain) done.message.append(std::move(link));
            done.status = status_t::COMPLETE;
        }
        reset();
        return done;
    }
};

}  // namespace tr::net
