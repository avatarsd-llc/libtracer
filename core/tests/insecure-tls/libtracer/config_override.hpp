/**
 * @file
 * @brief The insecure-TLS dev preset — the defaults with the SPEC `insecure` key of the `quic`
 *        and `webtransport` kinds honoured.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `default_config_t::kAllowInsecureTls` is `false`: a shipped node refuses a connection SPEC
 * that asks it to dial without verifying the peer's certificate. A development build that
 * reaches self-signed peers through a SPEC puts this directory on the include path
 * (`-DCMAKE_CXX_FLAGS=-I<repo>/core/tests/insecure-tls`); the `quic` CI workflow does so to
 * run `quic_test` and `webtransport_test` through the honoured arm. Never ship it.
 */
#pragma once

namespace tr::graph {

/** @brief The defaults, with the dev-only SPEC `insecure` key honoured. */
struct insecure_tls_config_t : default_config_t {
    static constexpr bool kAllowInsecureTls = true;
    /** @brief This preset replaces the test build's own, so it re-states the tiers that build
     *         compiles (`transport_can.cpp` / `self_heal_link.cpp` assert them). */
    static constexpr bool kBusLinks = true;
    static constexpr bool kSelfHealLinks = true; /**< @brief See @ref kBusLinks. */
};

using config_t = insecure_tls_config_t;

}  // namespace tr::graph
