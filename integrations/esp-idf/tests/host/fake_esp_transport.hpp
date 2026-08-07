/**
 * @file
 * @brief The host fake of `esp_transport_ws`'s inbound frame stream — the driver the
 *        WS *client* link's receive-path suite scripts.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The declarations the chip TU compiles against live in `fake_idf/esp_transport*.h`;
 * this is the model behind them, plus the levers a test needs: script the frames the
 * peer sends, wait for them to be drained, and read back what `esp_pthread_set_cfg` was
 * armed with.
 *
 * The ONE behaviour it reproduces exactly, because both bugs under test live on it
 * (transcribed from `components/tcp_transport/transport_ws.c`):
 *
 *   - a read is capped at the buffer the caller offers, and when a frame is bigger than
 *     that, the REST OF THAT SAME FRAME is served by the following reads —
 *     `ws_read` parses a new header only once `frame_state.bytes_remaining` reaches 0
 *     (transport_ws.c:729). Throughout, `_get_read_opcode` / `_get_fin_flag` keep
 *     reporting that one frame's header. So an oversized *unfragmented* message arrives
 *     as several reads that each look like a complete small message, which is how the
 *     tail of a dropped message reached the router as a bogus frame (#901);
 *   - `_get_read_payload_len` is the frame's TOTAL payload length, unchanged while the
 *     frame drains — the only signal that tells the two apart;
 *   - a zero-payload frame returns 0 from `esp_transport_read`, indistinguishable from a
 *     poll timeout (transport_ws.c:763).
 *
 * There is NO socket in this fake, at any layer: `esp_transport_get_socket` answers -1.
 * The ESP-IDF WebSocket plane must never use POSIX sockets (#947), and a fake that
 * opened one would quietly make the suite prove the wrong thing.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_transport_ws.h"

namespace fake_ws {

/** @brief One scripted inbound WebSocket frame, as the peer would put it on the wire. */
struct frame_t {
    ws_transport_opcodes_t op;      /**< @brief Its opcode (CONT for a continuation). */
    bool fin;                       /**< @brief Its FIN bit. */
    std::vector<std::byte> payload; /**< @brief Its payload, served across as many
                                     *         reads as the reader's buffer forces. */
};

/** @brief Build a frame of @p len bytes filled with a @p seed-derived pattern, so a
 *         delivered message can be checked byte-for-byte against what was sent. */
frame_t make_frame(ws_transport_opcodes_t op, bool fin, std::size_t len, std::uint8_t seed);

/** @brief Drop all fake state: script, connection bookkeeping, armed pthread config. */
void reset();

/**
 * @brief Queue @p frames for the link's recv thread to read.
 *
 * Appends to whatever is still unread, and wakes a thread parked in
 * `esp_transport_poll_read`. Until the first call, `poll_read` reports "no data", so a
 * test can construct the link, install its receiver, and only then start the stream.
 */
void push_frames(std::vector<frame_t> frames);

/** @brief True once every queued frame has been fully read. */
[[nodiscard]] bool drained();

/** @brief How many times a connection was opened (the link re-dials on every drop). */
[[nodiscard]] int connect_count();

/** @brief The `ws_path` the last dial requested. */
[[nodiscard]] std::string last_ws_path();

/**
 * @brief The `stack_size` the last ARMING `esp_pthread_set_cfg` carried, 0 if none.
 *
 * The #900 observable: on IDF this is the only way a `std::thread` gets a stack other
 * than `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`, so a knob that never reaches here is a
 * knob that never reaches the FreeRTOS task.
 */
[[nodiscard]] std::size_t armed_stack();

/** @brief The `thread_name` that arming config carried, empty if none. */
[[nodiscard]] std::string armed_name();

/**
 * @brief True iff the armed config was RESTORED afterwards (the last `set_cfg` put back
 *        a zero stack size) — the leak-onto-later-spawns half of the recipe.
 */
[[nodiscard]] bool cfg_restored();

}  // namespace fake_ws
