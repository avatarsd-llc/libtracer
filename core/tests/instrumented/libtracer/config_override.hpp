/**
 * @file
 * @brief The instrumented preset — the defaults with `graph_t`'s test/bench instrumentation
 *        counters compiled in (#1664), the two link modules opted in (#1670) and the
 *        test-only fault-injection hooks compiled in (#1719), and creation hooks allowed
 *        (#1945).
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
 * **The link modules.** `kBusLinks` and `kSelfHealLinks` default to `false` since v0.17.0
 * (#1670). The test build exercises both — the peer-named tier, CAN, and the S5 liveness
 * engine — so it opts in here, and `core/CMakeLists.txt` compiles the matching TUs
 * (`LIBTRACER_TRANSPORT_CAN`, `LIBTRACER_SELF_HEAL_LINKS`) by default for the same build. A
 * `bench/` build that binds this preset turns `LIBTRACER_SELF_HEAL_LINKS` on for the same
 * reason.
 *
 * **The fault-injection hooks.** `kFaultInjection` defaults to `false` (#1719): a shipped node
 * carries no `probe_fail_hook` check on its allocation path. The test build is the one build
 * that arms those hooks, so it opts in here. The `bench/` preset does not: a bench measures
 * the path a node ships.
 *
 * **Creation hooks.** `kCreationHooks` defaults to `false` on every profile (RFC-0030 §18 Q5):
 * a miss refuses and no vertex has a hook slot. The test build opts in so the opt-in creation
 * path is exercised; the refusal path is the same answer either way. `kSessionAdmission` (#1841)
 * is opted in for the same reason: the session-admission seam is exercised, and a build without
 * it has no seam to test.
 *
 * **A counting frame observer.** `frame_observer_t` defaults to a no-op (#2580). The test build
 * binds @ref tr::graph::counting_frame_observer_t, so `fwd_frame_observer_test` can prove the
 * router calls it once per inbound frame, with the link's name, before dispatch. Its tally is
 * per thread, so no other test's receive threads race on it.
 *
 * **The READ budget on, at its widest.** `kReadBudget` defaults to 0, off (#2774). The test
 * build turns it on so `fwd_read_budget_test` runs the refusal path and the RAM ratchets pin the
 * per-session cost with the budget on (every leg that binds its own fragment pins it off). 254,
 * the widest budget, so no other test's READs come near it: a test never closes a run, and only
 * a session past its 254th READ while a second session reads is refused.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tr::graph {

/**
 * @brief A frame observer that counts (#2580): how many frames this thread's router calls
 *        saw, and the link name of the last one.
 */
struct counting_frame_observer_t {
    /** @brief Frames observed on this thread. */
    static inline thread_local std::size_t calls = 0;
    /** @brief The last observed link name, truncated to the buffer. */
    static inline thread_local std::array<char, 64> last{};
    /** @brief How many bytes of @ref last are the name. */
    static inline thread_local std::size_t last_len = 0;
    /** @brief Count one frame on @p inbound_link_name. */
    static void on_frame(std::string_view inbound_link_name) noexcept {
        ++calls;
        last_len = inbound_link_name.copy(last.data(), last.size());
    }
    /** @brief The last observed link name. */
    static std::string_view last_name() noexcept { return {last.data(), last_len}; }
};

/** @brief The defaults, with the instrumentation counters, both link modules, the
 *         fault-injection hooks, creation hooks and the session-admission seam compiled in. */
struct instrumented_config_t : default_config_t {
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    /** @brief A POSIX host receives a borrowed UDP datagram on the stack (#2142). */
    static constexpr std::size_t kUdpStackScratchBytes = 16384;
    static constexpr bool kFaultInjection = true;
    static constexpr bool kCreationHooks = true;
    static constexpr bool kSessionAdmission = true;
    /** @brief Count inbound frames per thread (#2580). */
    using frame_observer_t = counting_frame_observer_t;
    /** @brief The READ budget on, at its widest (#2774). */
    static constexpr std::uint8_t kReadBudget = 254;
};

using config_t = instrumented_config_t;

}  // namespace tr::graph

#endif
