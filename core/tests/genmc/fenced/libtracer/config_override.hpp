/**
 * @file
 * @brief GenMC preset (#2120): the shipped QSBR header at the FENCED fence flavour, with
 *        two participant cells and two retired slots, so a model check stays small.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
#pragma once

namespace tr::graph {

/** @brief The smallest domain the harness's scenarios need. */
struct genmc_config_t : default_config_t {
    static constexpr std::size_t kQsbrParticipants = 2;
    static constexpr std::size_t kDeferredReleaseSlots = 2;
    static constexpr bool kQsbrAsymmetricFence = false;
    static constexpr bool kSingleCore = false;
};

using config_t = genmc_config_t;

}  // namespace tr::graph
