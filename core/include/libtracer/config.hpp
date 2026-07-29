/**
 * @file
 * @brief Per-target build configuration as plain C++ (ADR-0068): every compile-time
 *        knob is an `inline constexpr` constant or a `using` policy binding — never a
 *        preprocessor definition.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two renderings of one template exist:
 *
 *   - `config.hpp.in` — the single source of truth. CMake `configure_file`s it into the
 *     build tree with the target's knob values and prepends that directory to the PUBLIC
 *     include path, so the generated file SHADOWS this one for every TU of the build
 *     (one file per build ⇒ no per-TU `-D` mismatch, no ODR hazard).
 *   - the checked-in `config.hpp` — the default rendering, kept byte-identical by a
 *     configure-time drift gate (`FATAL_ERROR` on mismatch), so a raw `-Icore/include`
 *     consumer (the Cortex-M0 footprint gate, vendored source drops) builds with stock
 *     settings and no build-system participation.
 *
 * Adding a knob: add it HERE (both renderings via the gate), never as a macro in a
 * public header. The CMake cache variable / Kconfig option is the user-facing name; this
 * header is its C++ delivery vehicle.
 */
#pragma once

#include <cstddef>

namespace tr::graph {

struct allow_only_policy_t;  // security_acl.hpp — the ALLOW-only MCU profile (ADR-0020 subset)
struct full_acl_policy_t;    // security_acl.hpp — ordered first-match-per-bit with DENY

/**
 * @brief The number of lock stripes shared by every vertex in the process (#361 §2).
 *
 * A per-target config knob (RFC-0006: bounds are injected or per-target config, never
 * magic). CMake: `-DLIBTRACER_VERTEX_LOCK_STRIPES=8`; ESP-IDF: menuconfig
 * `CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES`. N stripes cost N lazily-allocated mutexes and
 * condvars process-wide; a small single-core node reclaims RAM at 4–8.
 */
inline constexpr std::size_t kVertexLockStripes = 16;

/**
 * @brief The target's selected ACL policy (ADR-0047 §1 build-time module set).
 *
 * Default: the ALLOW-only MCU profile. The CMake option `LIBTRACER_ACL_FULL=ON` rebinds
 * this alias to the full `security_acl` host policy (ordered first-match-per-bit with
 * DENY) — a target-configuration change, never an edit to `graph.cpp`.
 */
using acl_policy_t = allow_only_policy_t;

}  // namespace tr::graph
