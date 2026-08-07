/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_twai_onchip.h` — the node allocation
 *        config `twai_link.cpp` fills in.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation. IDF nests `io_cfg`'s fields in
 * an anonymous struct TYPE with a named member; a named type spelt the same way
 * takes the identical `node_config.io_cfg.tx = …` the TU writes, without relying
 * on the anonymous-struct extension in C++.
 *
 * The C spellings are IDF's, per the sibling fake_idf headers.
 */
#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_twai_types.h"

/** @brief On-chip TWAI node initialization configuration. */
typedef struct {
    /** @brief The controller's pads. */
    struct {
        gpio_num_t tx;                /**< @brief TX pin (to the transceiver). */
        gpio_num_t rx;                /**< @brief RX pin (from the transceiver). */
        gpio_num_t quanta_clk_out;    /**< @brief Quanta clock out, -1 to disable. */
        gpio_num_t bus_off_indicator; /**< @brief Bus-off indicator, -1 to disable. */
    } io_cfg;
    twai_clock_source_t clk_src;            /**< @brief Clock source, 0 for the default. */
    twai_timing_basic_config_t bit_timing;  /**< @brief Arbitration-phase timing. */
    twai_timing_basic_config_t data_timing; /**< @brief FD data-phase timing. */
    uint32_t timestamp_resolution_hz;       /**< @brief RX timestamp timebase, 0 to disable. */
    int8_t fail_retry_cnt;                  /**< @brief Hardware retry limit, -1 for forever. */
    uint32_t tx_queue_depth;                /**< @brief Depth of the driver's transmit queue. */
    int intr_priority;                      /**< @brief Interrupt priority. */
} twai_onchip_node_config_t;

/** @brief Allocate an on-chip TWAI node; @p node_ret receives the handle. */
esp_err_t twai_new_node_onchip(const twai_onchip_node_config_t* node_config,
                               twai_node_handle_t* node_ret);
