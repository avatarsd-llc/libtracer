/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_transport_ws.h` — the frame accessors the
 *        client link's receive loop steers by.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The enum values and the config layout are transcribed verbatim from
 * `components/tcp_transport/include/esp_transport_ws.h` (the config keeps only the
 * fields the link sets, in IDF's order). What matters for the receive-path contract is
 * the semantics of the three accessors, which fake_esp_transport.cpp reproduces from
 * `transport_ws.c`:
 *   - `esp_transport_ws_get_read_opcode` / `_get_fin_flag` report the CURRENT FRAME's
 *     header — re-reported unchanged on every read that serves a piece of that frame,
 *     because a new header is parsed only once `frame_state.bytes_remaining` reaches 0
 *     (transport_ws.c:729). A `fin` on its own therefore never means "the message ended
 *     here";
 *   - `esp_transport_ws_get_read_payload_len` is the frame's TOTAL payload length
 *     (`frame_state.payload_len`, transport_ws.c:1098), which is the only signal that
 *     separates "this read finished the frame" from "the buffer ran out mid-frame".
 */
#pragma once

#include <stdbool.h>

#include "esp_transport.h"

/** @brief RFC 6455 opcodes, as IDF spells them. */
typedef enum ws_transport_opcodes {
    WS_TRANSPORT_OPCODES_CONT = 0x00,
    WS_TRANSPORT_OPCODES_TEXT = 0x01,
    WS_TRANSPORT_OPCODES_BINARY = 0x02,
    WS_TRANSPORT_OPCODES_CLOSE = 0x08,
    WS_TRANSPORT_OPCODES_PING = 0x09,
    WS_TRANSPORT_OPCODES_PONG = 0x0a,
    WS_TRANSPORT_OPCODES_FIN = 0x80,
    WS_TRANSPORT_OPCODES_NONE = 0x100, /**< @brief No message received yet. */
} ws_transport_opcodes_t;

/** @brief WS transport configuration — the fields the link sets, in IDF's order. */
typedef struct {
    const char* ws_path;           /**< @brief HTTP path to upgrade. */
    const char* sub_protocol;      /**< @brief WS subprotocol. */
    const char* user_agent;        /**< @brief WS user agent. */
    const char* headers;           /**< @brief Extra CRLF-terminated header lines. */
    bool propagate_control_frames; /**< @brief False = IDF answers PING/CLOSE itself. */
} esp_transport_ws_config_t;

/** @brief Create the WS transport over @p parent_handle (does NOT take ownership). */
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent_handle);

/** @brief Apply @p config to @p t (recorded by the fake for assertions). */
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t,
                                      const esp_transport_ws_config_t* config);

/** @brief The CURRENT frame's opcode — unchanged across the reads that drain it. */
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t);

/** @brief The CURRENT frame's fin flag — likewise re-reported on every partial read. */
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t);

/** @brief The CURRENT frame's TOTAL payload length. */
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t);
