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
 *       shed-frame SUBSET every kind can answer; this block is the superset one kind
 *       publishes, and the two coexist by design — the naming of both follows the one
 *       introspection vocabulary in `core/STYLE.md` §Introspection (#1503). What is
 *       still true, and is the part worth keeping: no counter bump was added to core's
 *       hot `deliver_remote` path, and `fwd_router_t` grows no per-child accounting.
 *       Per-link attribution of core's delivery drops remains a future change.
 *
 * Threading: PLAIN fields, no atomics. Every one of them is read and written under
 * the owning link's EXISTING mutex (`esp_ws_client_link_t::write_m_`,
 * `httpd_ws_link_t::peers_m_`) — so the block costs no new lock, and the two 64-bit
 * timestamps stay coherent with the counts they belong to. `std::atomic<int64_t>`
 * would not be lock-free on RV32 anyway: it would take a hidden libatomic lock per
 * access, which is strictly worse than the mutex already being held.
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

namespace tr::net {

/**
 * @brief One connection's passive traffic counters — see the file comment for the
 *        threading contract (owner's mutex) and the message-granularity definition.
 */
struct link_counters_t {
    std::uint32_t rx_frames = 0; /**< @brief Messages delivered inbound. */
    std::uint32_t rx_bytes = 0;  /**< @brief Payload bytes delivered inbound. */
    std::uint32_t tx_frames = 0; /**< @brief Messages written outbound. */
    std::uint32_t tx_bytes = 0;  /**< @brief Payload bytes written outbound. */
    std::uint32_t tx_drops = 0;  /**< @brief Frames dropped toward this connection. */
    std::uint32_t rx_drops = 0;  /**< @brief Inbound discards (oversize / reassembly). */
    /** @brief `esp_timer_get_time()` of the last delivered message; -1 = never. */
    std::int64_t last_rx_us = -1;
    /** @brief `esp_timer_get_time()` when this connection came up; -1 = not connected. */
    std::int64_t connected_at_us = -1;
};

}  // namespace tr::net
