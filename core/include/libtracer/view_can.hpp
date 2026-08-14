/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * L1 view_can_frames (#55): header-elided CAN framing of one logical libtracer
 * payload onto a sequence of CAN data fields. Classic CAN carries up to 8 data
 * bytes per frame; CAN-FD up to 64. The TLV header is NOT carried — it is
 * reconstructed host-side from the 29-bit CAN ID scheme (can.hpp / ADR-0022), so
 * the existing CAN frames are byte-unchanged on the bus (zero added overhead).
 *
 * Mirrors the existing L1 view/rope primitives (`%view.hpp`, `%rope.hpp`): the split
 * is zero-copy — each CAN-frame window is a subview() over the source segment,
 * never a memcpy — and reassembly (`tr::net::can_reassembly_t`) chains those windows
 * back into a rope_t.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "libtracer/view.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) header-elided CAN framing: `can_frame_count` / `can_frame_at`.
 */

namespace tr::view {

/** @brief Whether a CAN data field is classic (≤8 bytes) or CAN-FD (≤64 bytes). */
enum class can_frame_mode_t : std::uint8_t {
    CLASSIC, /**< @brief Classic CAN 2.0: data field is 0–8 bytes. */
    FD,      /**< @brief CAN-FD: data field is 0–64 bytes (8/12/16/20/24/32/48/64 DLC). */
};

/** @brief Maximum CAN 2.0 (classic) data-field length, in bytes. */
inline constexpr std::size_t kCanClassicMaxData = 8;
/** @brief Maximum CAN-FD data-field length, in bytes. */
inline constexpr std::size_t kCanFdMaxData = 64;

/** @brief The maximum data-field length carried by one frame in @p mode. */
[[nodiscard]] constexpr std::size_t can_max_data(can_frame_mode_t mode) noexcept {
    return mode == can_frame_mode_t::FD ? kCanFdMaxData : kCanClassicMaxData;
}

/**
 * @brief Round @p len up to the next valid CAN-FD data-length-code (DLC) size.
 *
 * CAN-FD frames may only be 0–8, 12, 16, 20, 24, 32, 48, or 64 bytes, so a frame
 * of an in-between length is padded up to the next legal size on the wire. This
 * pure helper exposes that lattice; the actual padding is the SocketCAN binding's
 * (deferred-increment) job, so @ref can_frame_at windows stay the exact
 * logical chunk lengths (zero-copy), not padded.
 *
 * @param len The desired logical data length (`0..kCanFdMaxData`).
 * @return The smallest valid CAN-FD DLC size `>= len` (clamped to @ref kCanFdMaxData).
 */
[[nodiscard]] constexpr std::size_t can_fd_dlc_round_up(std::size_t len) noexcept {
    if (len <= 8) return len;
    if (len <= 12) return 12;
    if (len <= 16) return 16;
    if (len <= 20) return 20;
    if (len <= 24) return 24;
    if (len <= 32) return 32;
    if (len <= 48) return 48;
    return 64;
}

/**
 * @brief Number of CAN data-field windows @p payload occupies in @p mode.
 *
 * The ceiling division that defines the framing: a payload of `n` bytes takes
 * `ceil(n / can_max_data(mode))` frames. An empty payload yields zero frames.
 *
 * @param payload The contiguous source window to frame.
 * @param mode    Classic (≤8) or CAN-FD (≤64) framing.
 * @return The frame count; pair with @ref can_frame_at to walk the windows.
 */
[[nodiscard]] constexpr std::size_t can_frame_count(const view_t& payload,
                                                    can_frame_mode_t mode) noexcept {
    const std::size_t step = can_max_data(mode);
    return (payload.length + step - 1) / step;
}

/**
 * @brief The @p i-th CAN data-field window of @p payload in @p mode (zero-copy).
 *
 * Each window is a @ref view_t::subview over the source segment — never a memcpy.
 * Window `i` sits at `i * can_max_data(mode)` and runs to the mode's data-field
 * limit, except the tail window, which holds the remainder.
 *
 * @par The windows are DERIVED, never stored (#1110, #932)
 * Every window is a pure function of the payload length, the mode and the index, so
 * there is no table: this is O(1), allocation-free, and cannot fail.
 *
 * The framing used to accumulate its windows in a `std::vector<view_t>` with a THROWING
 * `push_back`, on a count that scales with a payload size the *sending peer* chooses;
 * under `-fno-exceptions` that is `abort()`, not a dropped frame. Bounding that growth
 * from an injected @ref tr::mem::block_source_t — the answer `tr::net::iov_table_t`
 * uses for the socket gather tables — was not available here: `block_array_t` requires a
 * trivially copyable, trivially destructible element and @ref view_t carries an
 * intrusive refcount. Storing a derivable table was the wrong half of the problem
 * anyway: with nothing stored there is no allocation to bound and no exhaustion path to
 * signal. #1110 deleted the table but kept a `view_can_frames_t` value around it;
 * #932 removed that too, because a class holding `(payload, mode)` plus a memo of a
 * one-line division is state the caller already has.
 *
 * @param payload The contiguous source window being framed.
 * @param mode    Classic (≤8) or CAN-FD (≤64) framing.
 * @param i       Frame index, `0 .. can_frame_count(payload, mode) - 1`.
 * @return A subview of @p payload; the tail window holds the remainder.
 */
[[nodiscard]] inline view_t can_frame_at(const view_t& payload, can_frame_mode_t mode,
                                         std::size_t i) {
    // Precondition, enforced in debug builds exactly as view_t::subview does.
    assert(i < can_frame_count(payload, mode));
    const std::size_t step = can_max_data(mode);
    const std::size_t off = i * step;
    const std::size_t rest = payload.length - off;
    return payload.subview(off, rest < step ? rest : step);
}

}  // namespace tr::view
