/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A fragment that still binds the withdrawn `reclaim_local_t` (#2127). `reclaim_local_refused`
 * compiles a TU against it and passes only when the build is refused with the migration note.
 */
#pragma once

namespace tr::graph {

struct withdrawn_reclaim_local_config_t : default_config_t {
    using reclaim_policy_t = reclaim_local_t;
};

using config_t = withdrawn_reclaim_local_config_t;

}  // namespace tr::graph
