/**
 * @file
 * @brief #1810 — a CI leg runs the configuration its name says: the bound LKV slot,
 *        reclamation policy and ACL policy are printed, and checked against the leg's
 *        expectation.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A leg binds its configuration through a `libtracer/config_override.hpp` on the include path.
 * A wrong `-I`, or a second fragment that shadows the first, rebuilds a DIFFERENT
 * configuration and the leg still goes green under its own name; that happened twice (#1692,
 * #1810). This test closes that gap. Each `core-ci.yml` leg sets the environment variables
 * below to the bindings its name states, and the test fails when the build bound anything
 * else:
 *
 *  - `LT_EXPECT_LKV_SLOT`       — `single_writer_slot_t` or `hazard_slot_t`;
 *  - `LT_EXPECT_RECLAIM_POLICY` — `reclaim_local_t`, `reclaim_strict_t` or `reclaim_qsbr_t`;
 *  - `LT_EXPECT_ACL_POLICY`     — `allow_only_policy_t` or `full_acl_policy_t`.
 *
 * A variable left unset is not checked, so a local build runs the test and only prints its
 * binding.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/lkv_slot.hpp"
#include "libtracer/reclaim.hpp"
#include "libtracer/security_acl.hpp"
#include "test_support.hpp"

namespace {

namespace g = tr::graph;
using tr::testing::check;

/** @brief The spelling a fragment uses for this build's LKV slot. */
constexpr std::string_view lkv_slot_name() {
    if constexpr (std::is_same_v<g::lkv_slot_t, g::hazard_slot_t>) {
        return "hazard_slot_t";
    } else if constexpr (std::is_same_v<g::lkv_slot_t, g::single_writer_slot_t>) {
        return "single_writer_slot_t";
    }
    return "unknown";
}

/** @brief The spelling a fragment uses for this build's reclamation policy. */
constexpr std::string_view reclaim_policy_name() {
    if constexpr (std::is_same_v<g::reclaim_policy_t, g::reclaim_qsbr_t>) {
        return "reclaim_qsbr_t";
    } else if constexpr (std::is_same_v<g::reclaim_policy_t, g::reclaim_strict_t>) {
        return "reclaim_strict_t";
    } else if constexpr (std::is_same_v<g::reclaim_policy_t, g::reclaim_local_t>) {
        return "reclaim_local_t";
    }
    return "unknown";
}

/** @brief The spelling a fragment uses for this build's ACL policy. */
constexpr std::string_view acl_policy_name() {
    if constexpr (std::is_same_v<g::acl_policy_t, g::full_acl_policy_t>) {
        return "full_acl_policy_t";
    } else if constexpr (std::is_same_v<g::acl_policy_t, g::allow_only_policy_t>) {
        return "allow_only_policy_t";
    }
    return "unknown";
}

/** @brief Checks one binding against the leg's expectation, when the leg states one. */
void expect(const char* variable, std::string_view bound) {
    const char* expected = std::getenv(variable);
    if (expected == nullptr) {
        std::printf("  %s unset: %.*s not checked\n", variable, static_cast<int>(bound.size()),
                    bound.data());
        return;
    }
    const std::string what =
        std::string(variable) + " = " + expected + ", bound " + std::string(bound);
    check(bound == expected, what);
}

}  // namespace

/** @brief Prints this build's binding and checks it against the leg's expectation. */
int main() {
    std::printf(
        "this build binds: lkv_slot_t = %.*s, reclaim_policy_t = %.*s, "
        "acl_policy_t = %.*s\n",
        static_cast<int>(lkv_slot_name().size()), lkv_slot_name().data(),
        static_cast<int>(reclaim_policy_name().size()), reclaim_policy_name().data(),
        static_cast<int>(acl_policy_name().size()), acl_policy_name().data());
    expect("LT_EXPECT_LKV_SLOT", lkv_slot_name());
    expect("LT_EXPECT_RECLAIM_POLICY", reclaim_policy_name());
    expect("LT_EXPECT_ACL_POLICY", acl_policy_name());
    return tr::testing::summary("config_binding");
}
