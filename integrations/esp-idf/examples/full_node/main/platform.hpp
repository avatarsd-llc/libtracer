/**
 * @file
 * @brief The per-target platform seam of the full_node example.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * app_main.cpp is a single portable TU; which implementation of these three
 * functions links in is a BUILD-SYSTEM decision (main/CMakeLists.txt picks
 * platform_esp.cpp on chip targets, platform_linux.cpp on the ESP-IDF `linux`
 * host target) — never an in-source #ifdef.
 */
#pragma once

#include <cstddef>
#include <span>

#include "libtracer/backend.hpp"

/**
 * @brief Bring the platform up (chips: NVS + netif + Wi-Fi station from
 *        menuconfig, skipped when no SSID is configured; linux host: a no-op).
 *
 * Returns false only on an unrecoverable bring-up error.
 */
bool platform_bring_up();

/**
 * @brief True on a real device (chip target): after the self-proof the node
 *        parks in the publish loop, serving peers forever.
 *
 * False on the linux host target: the app exits with the self-proof's status
 * (the CI gate).
 */
bool platform_is_device();

/**
 * @brief The node's RX byte seam over @p slab — a SYNCHRONISED pool, per target.
 *
 * The receive backend is handed to every transport endpoint, each of which allocates on
 * its own receive thread, and a delivered segment reclaims on whichever thread drops the
 * last reference — so it MUST be thread-safe (ADR-0060 §2; #770). A bare
 * `tr::mem::pool_t` is not: two threads can be handed the same slot.
 *
 * `tr::mem::synchronized_pool_t` keeps the pool (bounded slab, exhaustion = backpressure)
 * and makes the critical section a compile-time policy, because the target — not the
 * library — knows its concurrency model: chip targets get the interrupt-disable
 * `tr::esp::critical_pool_t` (a spinlock would invert priorities on a single-core
 * preemptive scheduler), the linux host target gets the spinlock `tr::mem::sync_pool_t`.
 * Both draw from the SAME caller-owned slab, so the one-slab bound is unchanged.
 *
 * The returned backend is a function-local static: it outlives the node and is
 * constructed once, on the first call, from the caller's @p slab.
 */
tr::mem::mem_backend_t& rx_backend(std::span<std::byte> slab, std::size_t slot_payload);

/**
 * @brief How many slots the seam returned by @ref rx_backend carved out of the slab.
 *
 * The pool type differs per target (the sync policy does), so the slot count — the
 * node's real RX bound, reported at bring-up — is read through this seam rather than off
 * a concrete pool type. Valid only after @ref rx_backend has been called.
 */
std::size_t rx_backend_slots();
