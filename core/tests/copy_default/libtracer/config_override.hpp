/**
 * @file
 * @brief The copy-by-default preset: `kDefaultShareHint = COPY` build-wide (#2226), on the
 *        default root the build names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `core/tests/CMakeLists.txt` builds three copies of the core over this directory, one per
 * default root, and links `share_hint_default_test` against each: `LT_COPY_DEFAULT_ROOT` is 0
 * for the slab pool, 1 for the static arena and 2 for the heap root. One configuration per
 * library: nothing else links them.
 */
#pragma once

#include <cstddef>

#ifndef LT_COPY_DEFAULT_ROOT
#error "LT_COPY_DEFAULT_ROOT names the default root: 0 slab pool, 1 static arena, 2 heap"
#endif

namespace tr::graph {

/** @brief The defaults, copying build-wide, on the root `LT_COPY_DEFAULT_ROOT` names. */
struct copy_default_config_t : default_config_t {
    static constexpr share_hint_t kDefaultShareHint = share_hint_t::COPY;
    static constexpr bool kSlabPool = LT_COPY_DEFAULT_ROOT == 0;
    static constexpr std::size_t kArenaBytes = LT_COPY_DEFAULT_ROOT == 0
                                                   ? default_config_t::kArenaBytes
                                               : LT_COPY_DEFAULT_ROOT == 1 ? 262144
                                                                           : 0;
};

using config_t = copy_default_config_t;

}  // namespace tr::graph
