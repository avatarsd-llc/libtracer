/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ACE RECORDS of a vertex's `:acl` (ADR-0020 / #81): the right bits, the ACE type,
 * the one honored flag, and the parsed entry itself. Data only — no evaluation, no wire
 * codec, no dependency on anything above `<cstdint>`.
 *
 * Why this is its own header and not part of `security_acl.hpp`, which is where #868 first
 * put it. The records are UPSTREAM of the graph vertex: `vertex_ext_t` stores a
 * `std::vector<ace_t>`, so whatever declares them is compiled by every net-plane TU. The
 * evaluation and the wire codec are DOWNSTREAM — `graph_t::acl_allows` and the `:acl`
 * field-write door call them, nothing in the vertex core does. Folding both halves into one
 * header forces the downstream half upstream: measured on this tree, it took
 * `security_acl.hpp` from 15 dependent TUs to 100, so editing an ACL evaluation rule would
 * have rebuilt the whole tree — the opposite of what #868 exists to achieve.
 *
 * So there are two ACL headers, and the seam between them is the real shape of the
 * dependency, not a compromise. What #868 actually asked for is satisfied: the ACE data has
 * ONE home and no longer straddles `vertex.hpp` and `security_acl.hpp`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @file
 * @brief `tr::graph` ACE records — the ACL data model (ADR-0020, docs/reference/05 §0x0A).
 */

namespace tr::graph {

/**
 * @brief One right bit of an ACE `access_mask` (docs/reference/05 §0x0A, ADR-0020).
 *
 * Single-bit values so a gate tests exactly one right; a stored mask may carry any
 * OR of them. `WRITE_ACL` is precisely the `admin` right (modify the ACL / delegate).
 */
enum class acl_right_t : std::uint32_t {
    READ = 0x01,        /**< @brief Read the vertex value / control fields. */
    WRITE = 0x02,       /**< @brief Write the vertex value / control fields (fan-in gate). */
    SUBSCRIBE = 0x04,   /**< @brief Append a `:subscribers[]` edge (fan-out gate). */
    CREATE = 0x08,      /**< @brief Create a child via `:children[]` (ADR-0017). */
    DELETE = 0x10,      /**< @brief Remove a child (reserved; no core surface yet). */
    READ_ACL = 0x20,    /**< @brief Read the `:acl` field. */
    WRITE_ACL = 0x40,   /**< @brief Modify the `:acl` field — the `admin` right. */
    WRITE_OWNER = 0x80, /**< @brief Transfer ownership (reserved; no core surface yet). */
};

/** @brief The one ACE flag the core subset honors: propagate to the subtree (ADR-0020). */
inline constexpr std::uint8_t kAceInherit = 0x1;

/** @brief An ACE's type (ADR-0020): ALLOW grants; DENY refuses (full policy only). */
enum class ace_type_t : std::uint8_t {
    ALLOW = 0, /**< @brief The ACE grants its mask's rights. */
    DENY = 1,  /**< @brief The ACE refuses them — evaluated only by `full_acl_policy_t`
                    (ADR-0050); the ALLOW-only profile rejects DENY at parse time. */
};

/**
 * @brief One parsed ACE of a vertex's `:acl` (ADR-0020 / #81).
 *
 * Evaluation is the pure per-target policy of ADR-0050 (`%security_acl.hpp`): the
 * default ALLOW-only MCU profile rejects a DENY ACE (or any flag bit beyond
 * `kAceInherit`) at write time with TYPE_MISMATCH, so stored ACEs never carry
 * semantics the selected evaluator would silently weaken; the full `security_acl`
 * host policy (LIBTRACER_ACL_FULL) stores DENY and evaluates ordered
 * first-match-per-bit.
 */
struct ace_t {
    ace_type_t type = ace_type_t::ALLOW; /**< @brief ALLOW or DENY (policy-gated at parse). */
    std::uint8_t flags = 0;              /**< @brief ACE flags; only `kAceInherit` is accepted. */
    std::vector<std::byte> subject;      /**< @brief Opaque subject token (ADR-0018); the special
                                              subject `"EVERYONE@"` matches any resolved subject. */
    std::uint32_t access_mask = 0; /**< @brief Granted rights (an OR of `acl_right_t` bits). */
    std::uint64_t expires_ns = 0;  /**< @brief Absolute expiry, ns since the UNIX epoch;
                                        0 = never expires. An expired ACE grants nothing. */
};

}  // namespace tr::graph
