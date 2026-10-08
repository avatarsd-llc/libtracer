/**
 * @file
 * @brief The footprint sentinels' configuration — a single-threaded bare-metal node (#1722).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `tools/cortexm0_footprint.py` and `tools/rv32_footprint.py` put this directory ahead of
 * `core/include`. It states the promise the removed `-DLIBTRACER_NO_ATOMIC` used to: the
 * sentinel node runs on one thread with no interrupt touching libtracer state, so the build's
 * one critical-section type guards nothing. On a core with no atomic read-modify-write
 * (Cortex-M0, rv32imc) the segment refcount then compiles to a plain load and store, as it did
 * under the macro; on one with it (rv32imac) the refcount stays one hardware RMW.
 *
 * A node with a second task, or an ISR that touches libtracer, binds an interrupt-masked
 * section here instead.
 *
 * The default root is the MCU one (`kSlabPool = false`): a static arena of `kArenaBytes`, which
 * the sentinel's RAM figure therefore includes. 2 KiB is what the fixture needs, not a
 * recommendation.
 */
#pragma once

#include <cstddef>

namespace tr::graph {

/** @brief The defaults, for a single-threaded MCU node with the MCU default root: a static
 *         arena, no heap (#1783). */
struct footprint_config_t : default_config_t {
    using guard_t = ::tr::no_guard_t;
    static constexpr bool kSlabPool = false;
    static constexpr std::size_t kArenaBytes = 2048;
};

using config_t = footprint_config_t;

}  // namespace tr::graph
