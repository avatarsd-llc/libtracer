/**
 * @file
 * @brief `tr::esp::portmux_sync_t` / `tr::esp::critical_pool_t` — the interrupt-disable
 *        critical-section policy for `tr::mem::synchronized_pool_t` on ESP-IDF.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0060 §2 names two synchronisation mechanisms for a shared `mem_backend_t`: a
 * spinlock for a multi-core host, an interrupt-disable critical section for a single-core
 * priority-preemptive target. `tr::mem::spin_sync_t` is the first; this is the second.
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
 * `graph_t`'s `value_backend`, `fwd_router_t`'s `flat` — each of which is reached from
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

namespace tr::esp {

/**
 * @brief The SINGLE-CORE MCU sync policy: an ESP-IDF portMUX critical section.
 *
 * `portENTER_CRITICAL_SAFE` disables interrupts (and, on a dual-core chip, takes the
 * portMUX spinlock as well, so the same policy stays correct on an S3), which is what
 * makes the guarded free-list update atomic against both task preemption and an ISR.
 */
struct portmux_sync_t {
    static constexpr bool is_isr_safe = true; /**< @brief Interrupts off — ISR-callable. */
    static constexpr bool is_nonblocking =
        true; /**< @brief Interrupt-disable — no heap, no syscall, no OS wait (#928). */
    static constexpr const char* name = "mem_critsec_pool"; /**< @brief Backend name. */

    /** @brief Enter the critical section (interrupts off for the O(1) free-list op). */
    void lock() noexcept { portENTER_CRITICAL_SAFE(&mux_); }
    /** @brief Leave the critical section. */
    void unlock() noexcept { portEXIT_CRITICAL_SAFE(&mux_); }

   private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

/**
 * @brief A bounded, caller-slab pool safe to inject at a shared seam on an ESP32.
 *
 * `tr::mem::synchronized_pool_t` specialised on @ref portmux_sync_t — the variant
 * ADR-0060 §2 names for a single-core priority-preemptive target. Construct it over the
 * application's own slab and inject it; see the file comment for the wiring.
 */
using critical_pool_t = tr::mem::synchronized_pool_t<portmux_sync_t>;

}  // namespace tr::esp
