/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_twai_types.h` (+ the `hal/twai_types.h`
 *        pieces it re-exports) — the exact surface `twai_link.cpp` names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation. Two deliberate departures from
 * the IDF originals, neither visible to the TU:
 *
 *   - the frame header's format bits (`ide`, `rtr`, …) sit DIRECTLY in the struct
 *     rather than inside the anonymous struct IDF nests them in. Anonymous structs
 *     are a compiler extension in C++ and this fake compiles under the suite's
 *     pedantic warning set; the member spelling `header.ide` — all the TU writes —
 *     is identical either way;
 *   - only the fields the TU (and the fake driver behind it) touch are carried.
 *
 * `twaifd_dlc2len` is transcribed verbatim from `hal/twai_types.h` minus its
 * HAL_ASSERT, because the RX path's length decision is computed from it and a
 * fake that rounded differently would prove the wrong thing.
 *
 * The C spellings are IDF's, per the sibling fake_idf headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief ESP TWAI controller handle (IDF's incomplete-struct pointer). */
typedef struct twai_node_base* twai_node_handle_t;

/** @brief Clock source selector; unused on the host, present so a `= {}` config compiles. */
typedef int twai_clock_source_t;

/** @brief Bitrate timing config, basic (simple) mode. */
typedef struct {
    uint32_t bitrate;     /**< @brief Bus bitrate in bits/second. */
    uint16_t sp_permill;  /**< @brief Sampling point, permill of the bit time. */
    uint16_t ssp_permill; /**< @brief Secondary sampling point, permill of the bit time. */
} twai_timing_basic_config_t;

/** @brief TWAI frame header / format. */
typedef struct {
    uint32_t id;        /**< @brief Arbitration identifier. */
    uint16_t dlc;       /**< @brief Data length code (FD coding; see twaifd_dlc2len). */
    uint32_t ide : 1;   /**< @brief Extended frame format (29-bit ID). */
    uint32_t rtr : 1;   /**< @brief Remote frame. */
    uint32_t fdf : 1;   /**< @brief FD format. */
    uint32_t brs : 1;   /**< @brief Bit-rate switch. */
    uint32_t esi : 1;   /**< @brief Error-state indicator. */
    uint64_t timestamp; /**< @brief RX timestamp / TX trigger time (a union in IDF). */
} twai_frame_header_t;

/** @brief One TWAI transaction: the descriptor plus the buffer it points at. */
typedef struct {
    twai_frame_header_t header; /**< @brief Metadata, excluding the data buffer. */
    uint8_t* buffer;            /**< @brief Data buffer for TX and RX. */
    size_t buffer_len;          /**< @brief Its length in bytes. */
    uint8_t tx_queue_priority;  /**< @brief Frame priority within the TX queue. */
} twai_frame_t;

/** @brief "TX done" event data. */
typedef struct {
    bool is_tx_success;                /**< @brief Whether the frame reached the bus. */
    const twai_frame_t* done_tx_frame; /**< @brief The frame the driver is finished with. */
} twai_tx_done_event_data_t;

/** @brief "RX done" event data (IDF carries nothing here either). */
typedef struct {
    int reserved; /**< @brief Placeholder: IDF's struct is empty, which C rejects. */
} twai_rx_done_event_data_t;

/**
 * @brief The event callbacks a node reports through — all in ISR context on chip.
 *
 * Only the two the TU registers are declared; IDF also carries `on_state_change`
 * and `on_error`, which `twai_link.cpp` leaves null.
 */
typedef struct {
    /** @brief TX-done hook; returns whether a higher-priority task was woken. */
    bool (*on_tx_done)(twai_node_handle_t handle, const twai_tx_done_event_data_t* edata,
                       void* user_ctx);
    /** @brief RX-done hook; returns whether a higher-priority task was woken. */
    bool (*on_rx_done)(twai_node_handle_t handle, const twai_rx_done_event_data_t* edata,
                       void* user_ctx);
} twai_event_callbacks_t;

/**
 * @brief Translate a TWAIFD DLC code to a byte length.
 *
 * Transcribed from `hal/twai_types.h`; the HAL_ASSERT on the DLC bound is dropped
 * because there is no HAL here to assert into.
 */
static inline uint16_t twaifd_dlc2len(uint16_t dlc) {
    return (dlc <= 8)    ? dlc
           : (dlc <= 12) ? (uint16_t)((dlc - 8) * 4 + 8)
           : (dlc <= 13) ? (uint16_t)((dlc - 12) * 8 + 24)
                         : (uint16_t)((dlc - 13) * 16 + 32);
}
