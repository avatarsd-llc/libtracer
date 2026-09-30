/**
 * @file
 * @brief `tr::net::link_counters_t` — the passive per-connection counter block the
 *        ESP-IDF WebSocket links keep, and the shape they hand out.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Scope, deliberately narrow: this is an `integrations/esp-idf` type, not a core
 * one. It stays a RICH, kind-specific block that only a host holding one of the two
 * CONCRETE link types it belongs to reads (@ref tr::net::esp_ws_client_link_t and
 * @ref tr::net::httpd_ws_link_t) — bytes, frames and connection timestamps that no
 * other transport kind can answer.
 *
 * @note The original wording here — "`transport_t` grows no `counters()` virtual" —
 *       was overtaken by #932 and is corrected rather than preserved: `transport_t`
 *       DOES carry a `virtual drop_stats()` (`core/include/libtracer/transport.hpp`),
 *       because a consumer holding only the interface otherwise lost all drop
 *       observability when it swapped ws for tcp or CAN. That virtual is the
 *       shed-frame SUBSET every kind can answer; this block is the superset, and the two
 *       coexist by design — the naming of both follows the one introspection vocabulary in
 *       `core/STYLE.md` §Introspection (#1503).
 *
 *       Since #1503 step 4 BOTH links that hold one of these blocks also override
 *       `drop_stats()`, each as a PROJECTION of the counters below rather than a second
 *       tally, so the generic seam and the rich block can no longer disagree. It used to
 *       be only @ref tr::net::httpd_ws_link_t, which meant a kind-agnostic consumer saw a
 *       client link's drops as zero.
 *
 *       What is still true, and is the part worth keeping: no counter bump was added to
 *       core's hot `deliver_remote` path, and `fwd_router_t` grows no PER-CHILD
 *       accounting — #1503 step 3 gave it per-SEAM counters, which is the ADR-0079 shape;
 *       per-link attribution of core's delivery drops remains a future change.
 *
 * Two halves, one of them optional (#1663):
 *   - The TRAFFIC half — `rx_frames`, `rx_bytes`, `tx_frames`, `tx_bytes` and
 *     `last_rx_us` — is bumped on the SUCCESS path, once per delivered or written
 *     message, under a lock and (on receive) with a timer read. `core/STYLE.md` has
 *     counters bump only on failure, so this half exists only when the application
 *     asks for it: `CONFIG_LIBTRACER_LINK_TRAFFIC_STATS`, default OFF. With it off the
 *     five fields are ABSENT (reading one does not compile), no success path takes a
 *     lock or reads the clock for them, and the client link carries no counter mutex.
 *     @ref tr::net::kLinkTrafficStats reports the setting.
 *   - The DROP half — `tx_drops`, `rx_drops` — and `connected_at_us` are always
 *     there. The drops bump only when a frame is lost, and the server's auth deadline
 *     is computed from `connected_at_us`, so neither is observability the application
 *     can opt out of.
 *
 *   This header reads `sdkconfig.h` itself, so every translation unit that includes it
 *   agrees on the layout.
 *
 * Threading: the block is a SNAPSHOT type. `httpd_ws_link_t` keeps one per session as
 * plain fields under its existing `peers_m_`. `esp_ws_client_link_t` assembles one in
 * `stats()`: the drop half and `connected_at_us` come from relaxed atomics that are
 * touched only on a failure or at a (re)connect, and the traffic half, when compiled
 * in, from plain fields under the link's own `st_m_`. The one 64-bit atomic there
 * (`connected_at_us`) is not lock-free on RV32; it is touched once per connect and once
 * per snapshot, never per frame.
 *
 * Units and definitions:
 *   - `rx_frames`/`rx_bytes` count DELIVERED MESSAGES and their payload bytes, not
 *     WebSocket fragments — the server bumps them at reassembly-complete delivery.
 *     That makes a client's `tx_frames` directly comparable with the server's
 *     `rx_frames` across a link, which is what an end-to-end test wants to assert.
 *   - `tx_drops`/`rx_drops` are CUMULATIVE since the connection was established
 *     (never a streak — the server keeps its 3-strike consecutive counter
 *     separately, and its semantics are untouched).
 *   - the two timestamps are `esp_timer_get_time()` microseconds since boot;
 *     -1 means "never" (no message yet / not connected).
 */
#pragma once

#include <cstdint>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

namespace tr::net {

/**
 * @brief Whether this image keeps the per-message TRAFFIC half of @ref link_counters_t —
 *        the application's compile-time choice, `CONFIG_LIBTRACER_LINK_TRAFFIC_STATS`
 *        (#1663). Off by default; the file comment says what each setting costs.
 */
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
inline constexpr bool kLinkTrafficStats = true;
#else
inline constexpr bool kLinkTrafficStats = false;
#endif

/**
 * @brief One connection's passive counters — see the file comment for the two halves,
 *        the threading contract and the message-granularity definition.
 */
struct link_counters_t {
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    std::uint32_t rx_frames = 0; /**< @brief Messages delivered inbound (traffic half). */
    std::uint32_t rx_bytes = 0;  /**< @brief Payload bytes delivered inbound (traffic half). */
    std::uint32_t tx_frames = 0; /**< @brief Messages written outbound (traffic half). */
    std::uint32_t tx_bytes = 0;  /**< @brief Payload bytes written outbound (traffic half). */
#endif
    std::uint32_t tx_drops = 0; /**< @brief Frames dropped toward this connection. */
    std::uint32_t rx_drops = 0; /**< @brief Inbound discards (oversize / reassembly). */
#ifdef CONFIG_LIBTRACER_LINK_TRAFFIC_STATS
    /** @brief `esp_timer_get_time()` of the last delivered message; -1 = never (traffic
     *         half). */
    std::int64_t last_rx_us = -1;
#endif
    /** @brief `esp_timer_get_time()` when this connection came up; -1 = not connected. */
    std::int64_t connected_at_us = -1;
};

}  // namespace tr::net
