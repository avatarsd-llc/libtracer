/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The probe TU for the #1158 spin-pool guard. Compiled TWICE by cmake/spin_pool_guard.cmake,
 * against two renderings of libtracer/config.hpp: it must compile where kSpinWaitSafe is true
 * and must FAIL where it is false. Not an add_executable() target — the check drives the
 * compiler directly, because what is under test is whether a build is rejected.
 */
#include <array>
#include <cstddef>
#include <span>

#include "libtracer/mem_pool.hpp"

/**
 * @file
 * @brief Instantiates `tr::mem::sync_pool_t`, the spelling the guard must reject.
 */

namespace {

/** @brief A slab the pool can carve; its size is irrelevant to the guard. */
std::array<std::byte, 256> g_slab{};

/** @brief The instantiation under test — declaring one is what trips the `static_assert`. */
tr::mem::sync_pool_t g_pool{std::span<std::byte>(g_slab), 64};

}  // namespace

/** @brief Keeps @ref g_pool odr-used so no toolchain can elide the instantiation. */
int main() { return g_pool.capacity() == 0 ? 1 : 0; }
