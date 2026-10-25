/**
 * @file
 * @brief CI preset — the ADR-0080 zero-cost reclamation policy bound (#894, #2300).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `reclaim=strict` leg of `core-ci.yml`'s build-test matrix puts this directory on the
 * include path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). It was the inline fixture of the separate
 * `build-test-reclaim-strict` job, kept member for member. `reclaim_strict_t` targets NARROW
 * bare metal with no preemption and no second core; this host leg only keeps the seam built.
 */
#pragma once

namespace tr::graph {

/** @brief The defaults, with `reclaim_strict_t` bound. */
struct reclaim_strict_config_t : default_config_t {
    using reclaim_policy_t = reclaim_strict_t;
    // The two link modules are opt-in since v0.17.0 (#1670); this leg's subject is
    // elsewhere, so it keeps the full test build's modules on.
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
    // The fault-injection tests run here too, so the hooks are compiled in (#1719).
    static constexpr bool kFaultInjection = true;
};

using config_t = reclaim_strict_config_t;

}  // namespace tr::graph
