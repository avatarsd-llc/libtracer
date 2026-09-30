/**
 * @file
 * @brief The instrumented preset — the defaults with `graph_t`'s test/bench instrumentation
 *        counters compiled in (#1664).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `default_config_t::kInstrumentCounters` is `false`, the lean choice a shipped node wants.
 * The core test build (`core/CMakeLists.txt`, when it builds `tests/`) and the `bench/` build's
 * `LIBTRACER_INSTRUMENT_COUNTERS` option put this directory FIRST on `libtracer`'s include
 * path, so the whole library and every consumer resolve this one fragment and agree on
 * `graph_t`'s layout.
 *
 * **It yields to a fragment the build already supplies.** A CI leg that binds its own
 * configuration (a reclamation policy, the bus module closed, ...) lists its fragment later on
 * the include path; that fragment is the configuration under test and must not be shadowed.
 * So when a later `libtracer/config_override.hpp` exists this file includes it INSTEAD and
 * binds nothing itself — the leg then runs with the counters closed out, and the assertions
 * that read them gate on `kInstrumentCounters`.
 */
#pragma once

#if defined(__has_include_next)
#if __has_include_next(<libtracer/config_override.hpp>)
#define LIBTRACER_INSTRUMENTED_PRESET_YIELDS 1
#endif
#endif

#if defined(LIBTRACER_INSTRUMENTED_PRESET_YIELDS)
#include_next <libtracer/config_override.hpp>
#else

namespace tr::graph {

/** @brief The defaults, with the two instrumentation counters compiled in. */
struct instrumented_config_t : default_config_t {
    static constexpr bool kInstrumentCounters = true;
};

using config_t = instrumented_config_t;

}  // namespace tr::graph

#endif
