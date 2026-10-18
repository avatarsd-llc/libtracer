/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The compile gate for the ESP-IDF critical-section pool policy (#770). The adapter is
 * a header (a template specialisation plus a policy struct), so nothing would compile it
 * until an application happened to instantiate one — and a header that never compiles is
 * a header that silently rots. This TU instantiates the specialisation explicitly, so
 * every chip build of the component type-checks the policy against
 * `tr::guard` and against the FreeRTOS port macros it wraps.
 *
 * It is only compiled for chip targets: the `linux` IDF target has no portMUX. Host
 * coverage of the SEAM (as opposed to this one adapter) is core/tests/mem_sync_policy_test.
 * Being the chip-only TU, it also pins the component's `kSingleCore` against `sdkconfig.h`.
 */

#include "libtracer_esp/critical_pool.hpp"

#include "libtracer/config.hpp"
#include "sdkconfig.h"

// A chip build binds kSingleCore exactly when FreeRTOS runs on one core (#2126): true on a
// second core would strip fences the grace period needs, and false here pays fences for nothing.
#if defined(CONFIG_FREERTOS_UNICORE)
static_assert(tr::graph::kSingleCore, "CONFIG_FREERTOS_UNICORE must bind kSingleCore = true");
#else
static_assert(!tr::graph::kSingleCore, "a multi-core chip build must bind kSingleCore = false");
#endif

// The QSBR participant count follows this component's kEdgePinSlots (#2111): inheriting core's
// 32 instead would cost 192 B of .bss on every chip build.
static_assert(tr::graph::kQsbrParticipants == tr::graph::kEdgePinSlots,
              "the ESP config's QSBR participant count must follow its kEdgePinSlots");

static_assert(tr::guard<tr::esp::critical_guard_t>,
              "the interrupt-masked guard must model the one critical-section trait");
static_assert(tr::esp::critical_pool_t::is_isr_safe,
              "a critical-section pool is ISR-safe; the trait must reach the backend");

/**
 * @brief Explicit instantiation — the whole point of this TU (see the file comment).
 *
 * At namespace scope, not inside `tr::esp`: an explicit instantiation must appear in a
 * namespace enclosing the template's own (`tr::mem`).
 */
template class tr::mem::synchronized_pool_t<tr::esp::critical_guard_t>;
