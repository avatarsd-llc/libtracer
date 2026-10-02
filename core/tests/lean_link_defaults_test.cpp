/**
 * @file
 * @brief #1670 — the two LINK MODULES are lean by default: `kBusLinks` and `kSelfHealLinks`
 *        are `false` unless a build opts in.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The maintainer ruling of 2026-09-30 (compile-time audit, B4) re-rules #1548's default: the
 * ADR-0044 bus tier (~2,078 B of rv32 flash) and the RFC-0014 §4 S5 liveness engine
 * (~4,336 B on an ESP32-C6 image) are carried only by a node that asks for them. What this
 * pins:
 *
 *  - **The shipped default is lean.** `default_config_t` binds both `false`, so a raw `-I`
 *    consumer, a vendored drop and every fragment that does not mention them get the lean
 *    image.
 *  - **Inheriting is not opting in.** A fragment that derives from `default_config_t` to set
 *    some OTHER knob — the ADR-0068 idiom every preset uses — inherits `false` for both.
 *  - **An opt-in is one line each and reaches the derived spellings.** A fragment that binds
 *    `true` is what `tr::net::kBusLinks` / `tr::net::kSelfHealLinks` then answer.
 *
 * The derived spellings in THIS build are printed, not asserted: the core test build opts in
 * through its preset (`core/tests/instrumented`), while a CI leg binding its own fragment may
 * not, and both are correct.
 */

#include <cstddef>
#include <cstdio>

#include "libtracer/config.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::default_config_t;
using tr::testing::check;

/** @brief A fragment in the ADR-0068 idiom that changes an unrelated knob and nothing else. */
struct unrelated_knob_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;
};

/** @brief A fragment that opts in to both link modules, as the test and bench presets do. */
struct opted_in_config_t : default_config_t {
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true;
};

/** @brief The stock default carries neither link module. */
void test_default_is_lean() {
    check(!default_config_t::kBusLinks,
          "default_config_t::kBusLinks is false — the bus tier is opt-in (#1670)");
    check(!default_config_t::kSelfHealLinks,
          "default_config_t::kSelfHealLinks is false — the S5 engine is opt-in (#1670)");
}

/** @brief Deriving to set another knob leaves both modules out. */
void test_inheriting_is_not_opting_in() {
    check(!unrelated_knob_config_t::kBusLinks,
          "a fragment that only sets an unrelated knob inherits kBusLinks = false");
    check(!unrelated_knob_config_t::kSelfHealLinks,
          "a fragment that only sets an unrelated knob inherits kSelfHealLinks = false");
}

/** @brief The opt-in is the member, and nothing else is needed at the config level. */
void test_opt_in_is_one_line_each() {
    check(opted_in_config_t::kBusLinks, "binding kBusLinks = true opts in to the bus tier");
    check(opted_in_config_t::kSelfHealLinks,
          "binding kSelfHealLinks = true opts in to the liveness engine");
    check(opted_in_config_t::kShareThresholdBytes == default_config_t::kShareThresholdBytes,
          "and every other knob keeps its default");
}

}  // namespace

/** @brief Runs the cases and reports this build's own binding. */
int main() {
    test_default_is_lean();
    test_inheriting_is_not_opting_in();
    test_opt_in_is_one_line_each();
    std::printf("this build binds: kBusLinks = %s, kSelfHealLinks = %s\n",
                tr::net::kBusLinks ? "true" : "false", tr::net::kSelfHealLinks ? "true" : "false");
    return tr::testing::summary("lean_link_defaults");
}
