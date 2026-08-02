/**
 * @file
 * @brief Platform seam, ESP-IDF `linux` (POSIX host) target: nothing to bring
 *        up — the host kernel's loopback carries the datagrams.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Selected by the build system (main/CMakeLists.txt); chip targets link
 * platform_esp.cpp instead.
 *
 * The RX seam here is the SPINLOCK-policy pool (`tr::mem::sync_pool_t`): this target is
 * a multi-core host, where an O(1) section under a spinlock is the cheap correct choice
 * and there are no interrupts to disable. Same slab, same bound, different policy — the
 * chip TU picks the interrupt-disable one (ADR-0060 §2).
 */

#include "libtracer/mem_pool.hpp"
#include "platform.hpp"

bool platform_bring_up() { return true; }

bool platform_is_device() { return false; }

namespace {

/** @brief The one RX pool, constructed on first use from the caller's slab. */
tr::mem::sync_pool_t* g_rx_pool = nullptr;

}  // namespace

tr::mem::mem_backend_t& rx_backend(std::span<std::byte> slab, std::size_t slot_payload) {
    static tr::mem::sync_pool_t pool{slab, slot_payload};
    g_rx_pool = &pool;
    return pool;
}

std::size_t rx_backend_slots() { return g_rx_pool != nullptr ? g_rx_pool->capacity() : 0; }
