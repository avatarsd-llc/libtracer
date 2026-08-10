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
 * never a memcpy — and reassembly chains those windows back into a rope_t.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

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
 *
 * @par The windows are DERIVED, never stored (#1110)
 * Every window sits at `i * step` and runs to `min(step, total - i * step)`, so the
 * whole table is a pure function of the payload length and the mode. This class
 * therefore keeps the payload window, the mode and the count — three words — and
 * computes each frame in @ref frame on demand.
 *
 * It used to accumulate the windows in a `std::vector<view_t>` with a THROWING
 * `push_back`, on a count that scales with a payload size the *sending peer* chooses;
 * under `-fno-exceptions` that is `abort()`, not a dropped frame. Bounding that growth
 * from an injected @ref tr::mem::block_source_t — the answer `tr::net::iov_table_t`
 * uses for the socket gather tables — is not available here: `block_array_t` requires a
 * trivially copyable, trivially destructible element and @ref view_t carries an
 * intrusive refcount. Storing a derivable table was the wrong half of the problem
 * anyway: with nothing stored there is no allocation to bound, no exhaustion path to
 * signal, and @ref split cannot fail.
 */
class view_can_frames_t {
   public:
    view_can_frames_t() = default;

    /**
     * @brief Split @p payload into CAN data-field windows for @p mode (zero-copy).
     *
     * O(1) and allocation-free: it records the payload and derives the count.
     *
     * @param payload The contiguous source window to frame.
     * @param mode    Classic (≤8) or CAN-FD (≤64) framing.
     * @return The frame sequence. An empty @p payload yields zero frames.
     */
    [[nodiscard]] static view_can_frames_t split(const view_t& payload,
                                                 can_frame_mode_t mode) noexcept {
        view_can_frames_t out;
        out.payload_ = payload;
        out.mode_ = mode;
        const std::size_t step = can_max_data(mode);
        out.count_ = (payload.length + step - 1) / step;
        return out;
    }

    /** @brief The framing mode these windows were split for. */
    [[nodiscard]] can_frame_mode_t mode() const noexcept { return mode_; }
    /** @brief Number of CAN frames the payload occupies. */
    [[nodiscard]] std::size_t frame_count() const noexcept { return count_; }

    /**
     * @brief The @p i-th CAN data-field window, computed on demand (zero-copy).
     *
     * @param i Frame index, `0 .. frame_count() - 1`.
     * @return A subview of the payload; the tail window holds the remainder.
     */
    [[nodiscard]] view_t frame(std::size_t i) const {
        // Precondition, enforced in debug builds exactly as view_t::subview does.
        assert(i < count_);
        const std::size_t step = can_max_data(mode_);
        const std::size_t off = i * step;
        const std::size_t rest = payload_.length - off;
        return payload_.subview(off, rest < step ? rest : step);
    }

    /**
     * @brief Chain the frame windows back into one logical @ref rope_t (zero-copy).
     * @return A rope whose links are the windows in order; empty for zero frames.
     */
    [[nodiscard]] rope_t to_rope() const {
        rope_t r;
        for (std::size_t i = 0; i < count_; ++i) r.append(frame(i));
        return r;
    }

   private:
    view_t payload_{};      /**< @brief The source window every frame is a subview of. */
    std::size_t count_ = 0; /**< @brief Frames the payload occupies, derived in `split`. */
    can_frame_mode_t mode_ = can_frame_mode_t::CLASSIC; /**< @brief Data-field limit. */
};

}  // namespace tr::view
