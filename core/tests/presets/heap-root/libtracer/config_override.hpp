/**
 * @file
 * @brief CI preset: the heap-rooted MCU default (#2090) on the host test build.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `kSlabPool = false` with `kArenaBytes = 0`: no slab pool and no static arena, so the platform
 * heap is the default root (`tr::mem::kHeapRoot`). Everything else is the instrumented test
 * preset's (`core/tests/instrumented`), so only the default root differs from the default leg.
 */
#pragma once

#include <cstddef>

namespace tr::graph {

/** @brief The instrumented test configuration with the heap as the default root. */
struct ci_heap_root_config_t : default_config_t {
    static constexpr bool kSlabPool = false;
    static constexpr std::size_t kArenaBytes = 0;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kCreationHooks = true;
};

using config_t = ci_heap_root_config_t;

}  // namespace tr::graph
