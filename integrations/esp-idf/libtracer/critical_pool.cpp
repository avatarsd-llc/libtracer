/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The compile gate for the ESP-IDF critical-section pool policy (#770). The adapter is
 * a header (a template specialisation plus a policy struct), so nothing would compile it
 * until an application happened to instantiate one — and a header that never compiles is
 * a header that silently rots. This TU instantiates the specialisation explicitly, so
 * every chip build of the component type-checks the policy against
 * `tr::mem::pool_sync_policy` and against the FreeRTOS port macros it wraps.
 *
 * It is only compiled for chip targets: the `linux` IDF target has no portMUX. Host
 * coverage of the SEAM (as opposed to this one adapter) is core/tests/mem_sync_policy_test.
 */

#include "libtracer_esp/critical_pool.hpp"

static_assert(tr::mem::pool_sync_policy<tr::esp::portmux_sync_t>,
              "the portMUX policy must model the pool's sync seam");
static_assert(tr::esp::critical_pool_t::is_isr_safe,
              "a critical-section pool is ISR-safe; the trait must reach the backend");

/**
 * @brief Explicit instantiation — the whole point of this TU (see the file comment).
 *
 * At namespace scope, not inside `tr::esp`: an explicit instantiation must appear in a
 * namespace enclosing the template's own (`tr::mem`).
 */
template class tr::mem::synchronized_pool_t<tr::esp::portmux_sync_t>;
