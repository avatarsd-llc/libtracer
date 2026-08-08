/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_twai.h` — the node-control calls
 *        `twai_link.cpp` names.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Declaration-compatible, not a re-implementation: there is no controller and no
 * bus. What the model behind these (fake_twai.hpp) does reproduce is the ONE
 * property the #383 pool and the #962 backpressure window both rest on — the
 * driver takes a frame POINTER, keeps it, and hands it back later through the
 * registered tx-done callback. On the host that "later" is a lever the test
 * pulls, which is what lets a suite stage a controller that never completes a
 * transmit (a bus-off node) without a CAN bus.
 *
 * The C spellings are IDF's, per the sibling fake_idf headers.
 */
#pragma once

#include "esp_err.h"
#include "esp_twai_types.h"

/** @brief Enable the node (it starts disabled). */
esp_err_t twai_node_enable(twai_node_handle_t node);

/** @brief Disable the node. */
esp_err_t twai_node_disable(twai_node_handle_t node);

/** @brief Delete a disabled node and release it. */
esp_err_t twai_node_delete(twai_node_handle_t node);

/** @brief Register @p cbs (called with @p user_data) on @p node. */
esp_err_t twai_node_register_event_callbacks(twai_node_handle_t node,
                                             const twai_event_callbacks_t* cbs, void* user_data);

/**
 * @brief Queue @p frame for transmission; the driver keeps the POINTER.
 *
 * @param timeout_ms The driver-side queue-full wait. `twai_link.cpp` always
 *        passes 0, because a held pool slot already implies driver-side room.
 */
esp_err_t twai_node_transmit(twai_node_handle_t node, const twai_frame_t* frame, int timeout_ms);

/** @brief Wait (bounded) for every queued frame to complete. */
esp_err_t twai_node_transmit_wait_all_done(twai_node_handle_t node, int timeout_ms);

/** @brief Copy the pending received frame out of the driver; ISR context on chip. */
esp_err_t twai_node_receive_from_isr(twai_node_handle_t node, twai_frame_t* rx_frame);
