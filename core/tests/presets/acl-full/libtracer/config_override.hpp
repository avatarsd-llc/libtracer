/**
 * @file
 * @brief CI preset — the instrumented test build with the full ACL host policy bound (#1722).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The `build-test (acl_full=ON, ...)` leg of `core-ci.yml` puts this directory on the include
 * path (`-DCMAKE_CXX_FLAGS=-I<this dir>`). It replaced the removed `-DLIBTRACER_ACL_FULL=ON`
 * configure option. The test build's instrumented preset yields to it, so it restates that
 * preset's three members as well.
 */
#pragma once

namespace tr::graph {

/** @brief The instrumented test configuration, evaluating ACLs with DENY, ordered. */
struct ci_acl_full_config_t : default_config_t {
    using acl_policy_t = full_acl_policy_t;
    static constexpr bool kInstrumentCounters = true;
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

using config_t = ci_acl_full_config_t;

}  // namespace tr::graph
