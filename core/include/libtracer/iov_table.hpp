/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * iov_table — the ONE nothrow overflow store behind every POSIX transport's
 * scatter-gather egress table (#848). Five copies of the same "stack array, else
 * grow a std::vector" pattern lived in transport_ws.cpp, transport_tcp.cpp and
 * transport_udp.cpp; all five grew with a THROWING resize/reserve on an entry
 * count the sending peer chooses (a rope's link count x its region count), which
 * on the -fno-exceptions MCU profile is abort(), not a dropped frame.
 */
#pragma once

#include <cstddef>
#include <span>

#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"

/**
 * @file
 * @brief @ref tr::net::iov_table_t — the nothrow scatter-gather entry table shared by the
 *        POSIX transports' egress (#848).
 */

namespace tr::net {

/**
 * @brief The inline scatter-gather entry count every POSIX transport carries on the
 *        stack before it reaches for the overflow store.
 *
 * A FWD forward/reply gathers only a few spans, so the common broadcast never allocates.
 * MEASURED (`bench_transport_iov`): the overflow fires at exactly **17 caller spans**,
 * because the frame header / length prefix takes entry 0.
 */
inline constexpr std::size_t kMaxInlineIov = 16;

/**
 * @brief A scatter-gather entry table: the caller's stack array while the entry count
 *        fits, a NOTHROW heap block when it does not.
 *
 * @tparam Entry The platform gather descriptor (`::iovec` for every POSIX transport).
 *         Templated so this header pulls in no system socket header — `posix_endpoint.hpp`
 *         only forward-declares `struct iovec`, and `tr::mem::block_array_t` needs the
 *         complete type. The transports instantiate `iov_table_t<::iovec>` in their `.cpp`.
 *
 * @par Why this exists rather than a `try_resize`
 * The overflow arm is the last unguarded per-frame allocation on the shipping forward path,
 * and its entry count is chosen by the SENDING peer: `ws_assembler_t::on_data` appends one
 * owning rope link per WS fragment with no cap on fragment count, and
 * `route_fwd_forward`'s rope arm gathers `link count x region count` entries out of it.
 * Fourteen fragments already exceed the inline bound. Growth here draws from a
 * `tr::mem::block_source_t` (ADR-0065) whose exhaustion answer is `nullptr`, so the caller
 * DROPS the frame the way both arms of `route_fwd_forward` already do — a truncated frame
 * on the wire is worse than no frame.
 *
 * The table owns its overflow block for its own (local, per-send) lifetime; the returned
 * pointer aliases either the caller's stack array or that block, so the table must outlive
 * the write.
 */
template <class Entry>
class iov_table_t {
   public:
    /**
     * @brief A table over the caller's stack array @p inline_vec, overflowing into @p src.
     *
     * @param inline_vec The caller's stack entry array (the no-allocation fast path).
     * @param src        The failable seam the overflow block is drawn from.
     */
    explicit iov_table_t(std::span<Entry> inline_vec,
                         mem::block_source_t& src = mem::heap_source()) noexcept
        : inline_(inline_vec), overflow_(src) {}

    /**
     * @brief Claim room for @p entries gather entries and return the (uninitialized) table.
     *
     * The entries themselves are filled by the caller; only the STORAGE is this class's
     * business. Honors the @ref tr::detail::probe_fail_hook test seam, so the drop leg is
     * reachable from a behavioural test without exhausting the host heap.
     *
     * @retval nullptr The seam refused — the caller MUST drop the frame (never truncate it).
     */
    [[nodiscard]] Entry* acquire(std::size_t entries) noexcept {
        if (entries <= inline_.size()) return inline_.data();
        if (!tr::detail::probe_hook_ok(entries * sizeof(Entry))) return nullptr;
        if (!overflow_.reserve(entries)) return nullptr;
        return overflow_.data();
    }

   private:
    std::span<Entry> inline_;            /**< @brief The caller's stack array. */
    mem::block_array_t<Entry> overflow_; /**< @brief The nothrow overflow block. */
};

}  // namespace tr::net
