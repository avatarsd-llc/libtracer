/**
 * @file
 * @brief Test fixture — the `single_writer_removed` STALE arm: a fragment written before #1718
 *        that still sets `kSingleWriter`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This must NOT compile, and must be refused by the removal tripwire in `config.hpp`, whose
 * message says the trait was removed. Were it accepted, the fragment would go on believing it
 * states a contract the library reads. Everything else here is the default, so no other
 * assertion objects.
 */
#pragma once

namespace tr::graph {

/** @brief A fragment that still states the single-writer contract removed by #1718. */
struct single_writer_removed_stale_config_t : default_config_t {
    static constexpr bool kSingleWriter = true;
};

using config_t = single_writer_removed_stale_config_t;

}  // namespace tr::graph
