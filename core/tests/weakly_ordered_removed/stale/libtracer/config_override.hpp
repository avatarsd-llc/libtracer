/**
 * @file
 * @brief Test fixture — the `weakly_ordered_removed` STALE arm: a fragment written before #1717
 *        that still sets `kWeaklyOrdered`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This must NOT compile, and must be refused by the removal tripwire in `config.hpp`, whose
 * message says the trait was removed. Were it accepted, a fragment that set it `false` would go
 * on believing it had waived the delivery-skip order assertion, which is now unconditional.
 * Everything else here is the default, so no other assertion objects.
 */
#pragma once

namespace tr::graph {

/** @brief A fragment that still claims a strongly-ordered target, the waiver #1717 removed. */
struct weakly_ordered_removed_stale_config_t : default_config_t {
    static constexpr bool kWeaklyOrdered = false;
};

using config_t = weakly_ordered_removed_stale_config_t;

}  // namespace tr::graph
