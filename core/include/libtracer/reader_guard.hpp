/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * reader_guard — DEPRECATED forwarding header (#1703). The guard vocabulary moved to the
 * layer-neutral `tr` namespace: `%guard.hpp` (freestanding) and `%guard_mutex.hpp` (hosted).
 */
#pragma once

#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"

/**
 * @file
 * @brief Deprecated: the `tr::graph` spellings of the guard vocabulary, kept as aliases for one
 *        release (#1703). Include `%guard.hpp` / `%guard_mutex.hpp` and name `tr::` instead.
 */

namespace tr::graph {

/**
 * @brief Deprecated alias of `tr::guard` (#1703); removed after one release.
 * @deprecated Name `tr::guard`.
 */
template <class G>
concept reader_guard = ::tr::guard<G>;

/**
 * @brief Deprecated alias of @ref tr::guard_scope_t (#1703); removed after one release.
 * @deprecated Name `tr::guard_scope_t`.
 */
template <class G>
using guard_scope_t = ::tr::guard_scope_t<G>;

}  // namespace tr::graph
