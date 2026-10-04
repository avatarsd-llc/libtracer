/**
 * @file
 * @brief The narrow-fan-out preset — the defaults with the inline snapshot width an ESP-IDF chip
 *        target binds (#1708).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `core/tests/CMakeLists.txt` builds a separate copy of the required core,
 * `libtracer_narrow_fanout`, with this directory FIRST on its include path, and links the
 * `inline_fanout_narrow` test against it, so the fan-out boundary is exercised at the width a
 * NARROW node ships with, not only at the host's. One configuration per library: nothing else
 * links it.
 */
#pragma once

#include <cstddef>

namespace tr::graph {

/** @brief The defaults, with the ESP-IDF chip targets' inline fan-out width. */
struct narrow_fanout_config_t : default_config_t {
    static constexpr std::size_t kInlineFanout = 2;
};

using config_t = narrow_fanout_config_t;

}  // namespace tr::graph
