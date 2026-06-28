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
 * Mirrors the existing L1 view/rope primitives (view.hpp, rope.hpp): the split
 * is zero-copy — each CAN-frame window is a subview() over the source segment,
 * never a memcpy — and reassembly chains those windows back into a rope_t.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief L1 (`tr::view`) header-elided CAN framing: `view_can_frames_t`.
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
 * (deferred-increment) job, so @ref view_can_frames_t windows stay the exact
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
 * @brief One logical payload framed (header-elided) as an ordered sequence of
 *        CAN data-field windows.
 *
 * Built by @ref split, which chops a source @ref view_t into windows no larger
 * than the mode's data-field limit — each window a zero-copy @ref view_t::subview
 * over the same segment. @ref to_rope chains the windows back into a @ref rope_t,
 * the reassembled payload. A single-frame payload (≤ the mode limit) yields one
 * window; a larger one yields a frame sequence whose tail window holds the
 * remainder.
 */
class view_can_frames_t {
   public:
    view_can_frames_t() = default;

    /**
     * @brief Split @p payload into CAN data-field windows for @p mode (zero-copy).
     *
     * @param payload The contiguous source window to frame.
     * @param mode    Classic (≤8) or CAN-FD (≤64) framing.
     * @return The ordered frame windows. An empty @p payload yields zero frames.
     */
    [[nodiscard]] static view_can_frames_t split(const view_t& payload, can_frame_mode_t mode) {
        view_can_frames_t out;
        out.mode_ = mode;
        const std::size_t step = can_max_data(mode);
        std::size_t off = 0;
        const std::size_t total = payload.length;
        while (off < total) {
            const std::size_t n = (total - off < step) ? (total - off) : step;
            out.frames_.push_back(payload.subview(off, n));
            off += n;
        }
        return out;
    }

    /** @brief The framing mode these windows were split for. */
    [[nodiscard]] can_frame_mode_t mode() const noexcept { return mode_; }
    /** @brief The ordered CAN data-field windows. */
    [[nodiscard]] const std::vector<view_t>& frames() const noexcept { return frames_; }
    /** @brief Number of CAN frames the payload occupies. */
    [[nodiscard]] std::size_t frame_count() const noexcept { return frames_.size(); }

    /**
     * @brief Chain the frame windows back into one logical @ref rope_t (zero-copy).
     * @return A rope whose links are the windows in order; empty for zero frames.
     */
    [[nodiscard]] rope_t to_rope() const {
        rope_t r;
        for (const auto& f : frames_) r.append(f);
        return r;
    }

   private:
    std::vector<view_t> frames_;
    can_frame_mode_t mode_ = can_frame_mode_t::CLASSIC;
};

}  // namespace tr::view
