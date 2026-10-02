/**
 * @file
 * @brief `tr::esp::critical_pool_t` — the interrupt-disable
 *        critical-section policy for `tr::mem::synchronized_pool_t` on ESP-IDF.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0060 §2 names two synchronisation mechanisms for a shared `mem_backend_t`: a
 * spinlock for a multi-core host, an interrupt-disable critical section for a single-core
 * priority-preemptive target. The host `tr::mutex_guard_t` is the first; this is the
 * second.
 * On a single-core MCU the spinlock is the WRONG one — a lower-priority task holding it
 * cannot run while a higher-priority task spins on it (unbounded priority inversion),
 * whereas a critical section cannot be preempted at all, so the ~120 ns free-list section
 * completes before anything else observes it.
 *
 * It lives in the ESP-IDF component, NOT in `core/`, because it needs FreeRTOS headers —
 * the same platform-TU rule `twai_link.cpp` follows (selection by which TU compiles,
 * never an in-source feature `#ifdef`). Which policy a target uses is a compile-time
 * choice (ADR-0068): the target's concurrency model is known at build time.
 *
 * WHERE IT GOES. Nothing defaults to it. It is opt-in construction at any shared byte
 * seam a node wants inside its own slab — `transport_vertex_t`'s `rx_backend` (#770),
 * `graph_t`'s single injected source (which since #873 phase 1 carries what the separate
 * `value_backend` argument used to), `fwd_router_t`'s `flat` — each of which is reached from
 * several threads and therefore may not take a bare `tr::mem::pool_t`:
 *
 * ```cpp
 * static std::byte g_rx_slab[12 * 1024];
 * tr::esp::critical_pool_t rx_pool{g_rx_slab, 1536};   // MTU-sized slots
 * tr::net::transport_vertex_t net{graph, router, "/net", &rx_pool};
 * ```
 *
 * BOUNDS. The slab is the caller's; the slot count is whatever fits. No knob here invents
 * a limit — exhaustion is `alloc` returning `nullptr`, i.e. backpressure.
 *
 * ISR USE. `portENTER_CRITICAL_SAFE` selects the task- or ISR-context primitive, so
 * `alloc`/`destroy` are callable from an ISR — hence `is_isr_safe == true`, which
 * `synchronized_pool_t` forwards as its own module-set trait (ADR-0047 §2). The section
 * must stay short: it is O(1) free-list pointer work and calls nothing that can block.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "libtracer/mem_pool.hpp"
#include "libtracer_esp/critical_guard.hpp"

namespace tr::esp {

/**
 * @brief A bounded, caller-slab pool safe to inject at a shared seam on an ESP32.
 *
 * `tr::mem::synchronized_pool_t` specialised on `tr::esp::critical_guard_t` (the one
 * interrupt-masked guard this component binds for the LKV slot too) — the variant
 * ADR-0060 §2 names for a single-core priority-preemptive target. Construct it over the
 * application's own slab and inject it; see the file comment for the wiring.
 */
using critical_pool_t = tr::mem::synchronized_pool_t<critical_guard_t>;

}  // namespace tr::esp
