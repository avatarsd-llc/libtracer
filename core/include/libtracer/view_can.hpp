/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One-release alias header (#1725). CAN framing is transport framing, so it moved
 * from the view layer (`tr::view`) to the net plane's CAN namespace (`tr::net::can`)
 * in `%can_framing.hpp`. This header keeps the old include path and the old
 * `tr::view::` spellings compiling for one release; it holds no code of its own and
 * is removed in the next release. The using-declarations below name `tr::net` from
 * `tr::view`, which core/STYLE.md hard rule 1 forbids; they are a deliberate, time-boxed
 * exception that exists only for the alias window, and no core header or source uses them.
 *
 * Migration: include `libtracer/can_framing.hpp` and spell the names
 * `tr::net::can::can_frame_mode_t`, `tr::net::can::can_frame_count`,
 * `tr::net::can::can_frame_at`, `tr::net::can::can_max_data`,
 * `tr::net::can::can_fd_dlc_round_up`, `tr::net::can::kCanClassicMaxData` and
 * `tr::net::can::kCanFdMaxData`.
 */
#pragma once

#include "libtracer/can_framing.hpp"

/**
 * @file
 * @brief Deprecated alias header: the old `tr::view` CAN-framing names, forwarded to
 *        `tr::net::can` (`%can_framing.hpp`) for one release.
 */

namespace tr::view {

using tr::net::can::can_fd_dlc_round_up;
using tr::net::can::can_frame_at;
using tr::net::can::can_frame_count;
using tr::net::can::can_frame_mode_t;
using tr::net::can::can_max_data;
using tr::net::can::kCanClassicMaxData;
using tr::net::can::kCanFdMaxData;

}  // namespace tr::view
