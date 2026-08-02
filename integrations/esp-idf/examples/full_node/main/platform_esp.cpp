/**
 * @file
 * @brief Platform seam, chip targets: minimal menuconfig-driven Wi-Fi station
 *        bring-up (NVS + esp_netif + esp_wifi).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * If no SSID is configured (the CI build-gate default) Wi-Fi is skipped
 * entirely — the self-proof runs over lwIP's loopback netif, no radio needed.
 * With credentials set, the node joins the LAN and its UDP listener is
 * reachable by a real host peer (the on-silicon FWD e2e).
 *
 * Selected by the build system (main/CMakeLists.txt); the linux host target
 * links platform_linux.cpp instead.
 *
 * This TU also supplies the node's RX byte seam: `tr::esp::critical_pool_t`, the
 * interrupt-disable variant of the synchronised pool (ADR-0060 §2, #770). A receive
 * backend is shared across every endpoint's receive thread and reclaims on whichever
 * thread drops the last reference, so it must be thread-safe; on a single-core
 * priority-preemptive scheduler the critical section — not a spinlock, which a
 * higher-priority task would spin on while its lower-priority holder cannot run — is the
 * mechanism. The host TU wires the spinlock policy over the same slab.
 */

#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "libtracer_esp/critical_pool.hpp"
#include "nvs_flash.h"
#include "platform.hpp"
#include "sdkconfig.h"

bool platform_bring_up() {
    // NVS backs the Wi-Fi driver's calibration/config storage.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return false;

    if (esp_netif_init() != ESP_OK) return false;
    if (esp_event_loop_create_default() != ESP_OK) return false;

    // No SSID configured => loopback-only node (the CI build gate); done.
    if (CONFIG_FULL_NODE_WIFI_SSID[0] == '\0') return true;

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), CONFIG_FULL_NODE_WIFI_SSID,
                 sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), CONFIG_FULL_NODE_WIFI_PASSWORD,
                 sizeof(cfg.sta.password) - 1);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    // Fire-and-forget connect: the node's self-proof runs on loopback and does
    // not gate on DHCP; the LAN listener becomes reachable once association
    // completes (esp_netif's default handlers drive DHCP).
    (void)esp_wifi_connect();
    return true;
}

bool platform_is_device() { return true; }

namespace {

/** @brief The one RX pool, constructed on first use from the caller's slab. */
tr::esp::critical_pool_t* g_rx_pool = nullptr;

}  // namespace

tr::mem::mem_backend_t& rx_backend(std::span<std::byte> slab, std::size_t slot_payload) {
    static tr::esp::critical_pool_t pool{slab, slot_payload};
    g_rx_pool = &pool;
    return pool;
}

std::size_t rx_backend_slots() { return g_rx_pool != nullptr ? g_rx_pool->capacity() : 0; }
